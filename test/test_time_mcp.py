#!/usr/bin/env python3
"""Protocol and tool tests for time-mcp.

Usage: python3 test_time_mcp.py [path/to/time-mcp]

Each check starts the server, writes request lines to its standard input and
reads the reply lines. The clock is fixed with TIME_MCP_TEST_NOW so results
are exact.
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BINARY = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "time-mcp")

MODERN = "2026-07-28"
LEGACY = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"]
PV = "io.modelcontextprotocol/protocolVersion"
CAPS = "io.modelcontextprotocol/clientCapabilities"
INFO = "io.modelcontextprotocol/serverInfo"
MODERN_META = {PV: MODERN, CAPS: {}}

# 2026-10-01 10:57:59 UTC, a Thursday.
NOW = 1790852279
# Noon UTC on the days United States clocks change in 2026.
SPRING_FORWARD = 1772971200  # 2026-03-08
FALL_BACK = 1793534400  # 2026-11-01

failures = []
checks = 0


def check(condition, label, detail=""):
    global checks
    checks += 1
    if not condition:
        failures.append(label)
        print("FAIL: %s %s" % (label, detail))


def launch(data, args=(), now=NOW, env_extra=None):
    """Runs the server on `data` (bytes) and returns the finished process."""
    env = dict(os.environ)
    env.pop("TZDIR", None)
    env.pop("TZ", None)
    if now is not None:
        env["TIME_MCP_TEST_NOW"] = str(now)
    if env_extra:
        env.update(env_extra)
    return subprocess.run([BINARY] + list(args), input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          env=env, timeout=30)


def exchange(messages, args=("--local-timezone", "America/Los_Angeles"), now=NOW):
    """Sends JSON messages, one per line, and returns the parsed reply lines."""
    data = "".join(json.dumps(m) + "\n" for m in messages).encode()
    done = launch(data, args, now)
    check(done.returncode == 0, "exit status 0", "got %d: %s" % (done.returncode, done.stderr.decode()))
    return [json.loads(line) for line in done.stdout.decode().split("\n")[:-1]]


def request(ident, method, params=None, meta=None):
    message = {"jsonrpc": "2.0", "id": ident, "method": method}
    if params is not None or meta is not None:
        message["params"] = dict(params or {})
        if meta is not None:
            message["params"]["_meta"] = meta
    return message


def initialize(version, ident=0):
    return request(ident, "initialize", {"protocolVersion": version, "capabilities": {},
                                         "clientInfo": {"name": "test", "version": "1"}})


def call(ident, name, arguments, meta=None):
    return request(ident, "tools/call", {"name": name, "arguments": arguments}, meta)


def call_one(name, arguments, now=NOW, version="2025-11-25"):
    """One legacy tools/call; returns its result object."""
    replies = exchange([initialize(version), call(1, name, arguments)], now=now)
    return replies[1]["result"]


def test_command_line():
    done = launch(b"", ["--version"])
    check(done.returncode == 0 and done.stdout.decode().startswith("time-mcp "), "--version")
    done = launch(b"", ["--help"])
    check(done.returncode == 0 and b"--local-timezone" in done.stdout, "--help")
    done = launch(b"", ["--nope"])
    check(done.returncode == 2 and done.stdout == b"", "unknown option exits 2")
    done = launch(b"", ["--local-timezone"])
    check(done.returncode == 2, "--local-timezone without a value exits 2")
    for bad in ["Foo/Bar", "../etc/passwd", "/etc/passwd", "", "zone.tab"]:
        done = launch(b"", ["--local-timezone", bad])
        check(done.returncode == 2 and done.stdout == b"" and done.stderr != b"",
              "invalid --local-timezone exits 2", repr(bad))
    done = launch(b"", [], env_extra={"TZDIR": "/nonexistent/zoneinfo"})
    check(done.returncode == 2, "no zone database exits 2")
    # Without the option the machine's own zone is found.
    done = launch((json.dumps(request(1, "tools/list")) + "\n").encode())
    check(done.returncode == 0 and b"get_current_time" in done.stdout, "starts without --local-timezone")
    # Both spellings of the option reach the tool descriptions.
    for args in (["--local-timezone", "Europe/Warsaw"], ["--local-timezone=Europe/Warsaw"]):
        replies = exchange([request(1, "tools/list")], args=args)
        text = json.dumps(replies[0])
        check(text.count("Use 'Europe/Warsaw' as local timezone") == 3, "local zone in the descriptions", str(args))


def test_legacy_handshake():
    for version in LEGACY:
        replies = exchange([initialize(version)])
        result = replies[0]["result"]
        check(result["protocolVersion"] == version, "initialize echoes " + version)
        check(result["capabilities"] == {"tools": {"listChanged": False}}, "capabilities")
        check(result["serverInfo"]["name"] == "time-mcp" and result["serverInfo"]["version"], "serverInfo")
        check("resultType" not in result, "legacy results carry no resultType")
    for version in ["1999-01-01", MODERN, ""]:
        replies = exchange([initialize(version)])
        check(replies[0]["result"]["protocolVersion"] == LEGACY[0], "initialize answers the newest legacy revision",
              version)
    replies = exchange([request(0, "initialize", {})])
    check(replies[0]["result"]["protocolVersion"] == LEGACY[0], "initialize without a version")


def test_base_protocol():
    replies = exchange([
        initialize("2025-11-25"),
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        request(1, "ping"),
        request("two", "nope/nothing"),
        {"jsonrpc": "2.0", "method": "nope/notification"},
        request(3, "resources/list"),
        {"jsonrpc": "2.0", "id": 4, "result": {}},
        request(5, "ping", {"extra": 1}),
    ])
    check(len(replies) == 5, "notifications and responses get no reply", str(len(replies)))
    check(replies[1] == {"jsonrpc": "2.0", "id": 1, "result": {}}, "ping")
    check(replies[2]["id"] == "two" and replies[2]["error"]["code"] == -32601, "unknown method is -32601")
    check(replies[3]["error"]["code"] == -32601, "resources/list is -32601")
    check(replies[4]["id"] == 5 and replies[4]["result"] == {}, "ping with params")

    done = launch(b'{"jsonrpc":"2.0","id":1,"method":"ping"\n[1,2\nnot json\n{"jsonrpc":"2.0","id":2,"method":"ping"}\n')
    replies = [json.loads(line) for line in done.stdout.decode().split("\n")[:-1]]
    check(len(replies) == 4, "one reply per line", str(len(replies)))
    for reply in replies[:3]:
        check(reply["id"] is None and reply["error"]["code"] == -32700, "unparsable line is -32700")
    check(replies[3]["result"] == {}, "the server recovers after bad lines")

    replies = exchange([
        5,
        "text",
        {"jsonrpc": "2.0", "id": None, "method": "ping"},
        {"jsonrpc": "2.0", "id": 1.5, "method": "ping"},
        {"jsonrpc": "2.0", "id": [1], "method": "ping"},
        {"jsonrpc": "2.0", "id": 7},
        {"jsonrpc": "2.0", "id": 8, "method": 9},
        {"jsonrpc": "2.0", "id": 9, "method": "ping", "params": [1]},
        {"jsonrpc": "2.0", "id": 10, "method": "tools/list", "params": "x"},
    ])
    check(len(replies) == 9, "every malformed request is answered", str(len(replies)))
    for reply in replies[:5]:
        check(reply["id"] is None and reply["error"]["code"] == -32600, "invalid request is -32600", str(reply))
    check(replies[5]["id"] == 7 and replies[5]["error"]["code"] == -32600, "no method is -32600")
    check(replies[6]["id"] == 8 and replies[6]["error"]["code"] == -32600, "non-string method is -32600")
    check(replies[7]["id"] == 9 and replies[7]["error"]["code"] == -32602, "array params is -32602")
    check(replies[8]["id"] == 10 and replies[8]["error"]["code"] == -32602, "string params is -32602")

    # Ids come back exactly as sent.
    for ident in [0, -3, 9007199254740993, "abc", "", "\u00e9\"\\"]:
        replies = exchange([request(ident, "ping")])
        check(replies[0]["id"] == ident and type(replies[0]["id"]) is type(ident), "id echoed", repr(ident))


def test_transport():
    ping = b'{"jsonrpc":"2.0","id":%d,"method":"ping"}'
    # Every complete line is answered before exit, and so is a last line
    # without a newline.
    done = launch(b"\n".join(ping % n for n in range(50)))
    replies = [json.loads(line) for line in done.stdout.decode().split("\n")[:-1]]
    check([r["id"] for r in replies] == list(range(50)), "all lines answered in order at end of input")
    done = launch(ping % 1 + b"\r\n\r\n\n" + ping % 2 + b"\r\n")
    check(len(done.stdout.splitlines()) == 2, "CRLF line ends and blank lines")
    check(b"\r" not in done.stdout, "replies end with a bare newline")
    done = launch(b"")
    check(done.returncode == 0 and done.stdout == b"", "empty input")

    # A line over the limit is refused and the next one is still served.
    huge = b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"x":"' + b"a" * (2 << 20) + b'"}}'
    done = launch(huge + b"\n" + ping % 2 + b"\n")
    replies = [json.loads(line) for line in done.stdout.decode().split("\n")[:-1]]
    check(len(replies) == 2 and replies[0]["error"]["code"] == -32700 and replies[1]["id"] == 2, "oversized line")
    done = launch(huge)
    check(done.returncode == 0 and done.stdout == b"", "oversized line cut off by end of input")

    # Invalid UTF-8 and control characters never reach the output.
    done = launch(b'{"jsonrpc":"2.0","id":"\xff\xfe","method":"ping"}\n' + ping % 3 + b"\n")
    replies = [json.loads(line) for line in done.stdout.decode().split("\n")[:-1]]
    check(replies[0]["error"]["code"] == -32700 and replies[1]["id"] == 3, "invalid UTF-8 is a parse error")
    replies = exchange([call(1, "get_current_time", {"timezone": "a\nb\u2028c"})])
    check(replies[0]["result"]["isError"] is True, "newline in a zone name")

    # Batches (revision 2025-03-26).
    done = launch(b'[' + ping % 1 + b',{"jsonrpc":"2.0","method":"notifications/x"},' + ping % 2 + b',7]\n'
                  b'[]\n[{"jsonrpc":"2.0","method":"notifications/x"}]\n' + ping % 3 + b"\n")
    replies = [json.loads(line) for line in done.stdout.decode().split("\n")[:-1]]
    check(len(replies) == 3, "batch reply count", str(len(replies)))
    check(isinstance(replies[0], list) and [r.get("id") for r in replies[0]] == [1, 2, None], "batch replies")
    check(replies[0][2]["error"]["code"] == -32600, "bad batch member")
    check(replies[1]["error"]["code"] == -32600, "empty batch is -32600")
    check(replies[2]["id"] == 3, "a batch of notifications gets no reply")
    done = launch(b"[" + b",".join(ping % n for n in range(100)) + b"]\n[" + b"1," * 100000 + b"1]\n")
    replies = [json.loads(line) for line in done.stdout.decode().split("\n")[:-1]]
    check(len(replies) == 2 and len(replies[0]) == 100, "a batch of 100 is served")
    check(replies[1]["id"] is None and replies[1]["error"]["code"] == -32600, "an oversized batch gets one -32600",
          str(replies[1])[:100])


ZONE_TIME_KEYS = ["timezone", "datetime", "day_of_week", "is_dst"]


def check_tools(tools, with_output_schema, label):
    check([t["name"] for t in tools] == ["get_current_time", "convert_time"], label + ": names and order")
    for tool in tools:
        check(tool["annotations"] == {"readOnlyHint": True, "destructiveHint": False, "idempotentHint": True,
                                      "openWorldHint": False}, label + ": annotations")
        check(tool["inputSchema"]["type"] == "object", label + ": inputSchema")
        check(("outputSchema" in tool) == with_output_schema, label + ": outputSchema presence")
        check(bool(tool["description"]) and bool(tool["title"]), label + ": description and title")
    first, second = tools
    check(first["inputSchema"]["required"] == ["timezone"], label + ": required")
    check(second["inputSchema"]["required"] == ["source_timezone", "time", "target_timezone"], label + ": required")
    check(list(second["inputSchema"]["properties"]) == ["source_timezone", "time", "target_timezone"],
          label + ": property order")
    text = json.dumps(tools)
    check("America/San_Francisco" not in text and "America/Los_Angeles" in text, label + ": example zones")
    if with_output_schema:
        check(first["outputSchema"]["required"] == ZONE_TIME_KEYS, label + ": output keys")
        check(second["outputSchema"]["required"] == ["source", "target", "time_difference"], label + ": output keys")
        for side in ("source", "target"):
            check(second["outputSchema"]["properties"][side]["required"] == ZONE_TIME_KEYS, label + ": output keys")


def test_tools_list():
    for version in LEGACY:
        replies = exchange([initialize(version), request(1, "tools/list"), request(2, "tools/list", {"cursor": None})])
        result = replies[1]["result"]
        check(list(result) == ["tools"], version + ": tools/list result keys", str(list(result)))
        check_tools(result["tools"], version >= "2025-06-18", version)
        check(replies[2]["result"] == result, version + ": the list is the same every time")
    # Without a handshake a legacy request is served under the newest legacy
    # revision.
    replies = exchange([request(1, "tools/list")])
    check_tools(replies[0]["result"]["tools"], True, "no handshake")


def test_get_current_time():
    expected = {"timezone": "America/New_York", "datetime": "2026-10-01T06:57:59-04:00", "day_of_week": "Thursday",
                "is_dst": True}
    for version in LEGACY:
        result = call_one("get_current_time", {"timezone": "America/New_York"}, version=version)
        check(result["isError"] is False, version + ": isError false")
        check(len(result["content"]) == 1 and result["content"][0]["type"] == "text", version + ": one text block")
        text = result["content"][0]["text"]
        check(text == json.dumps(expected, indent=2), version + ": text is the result as indented JSON", text)
        if version >= "2025-06-18":
            check(result["structuredContent"] == expected, version + ": structuredContent")
            check(list(result["structuredContent"]) == ZONE_TIME_KEYS, version + ": key order")
        else:
            check("structuredContent" not in result, version + ": no structuredContent")

    cases = [
        ("UTC", NOW, "2026-10-01T10:57:59+00:00", "Thursday", False),
        ("utc", NOW, "2026-10-01T10:57:59+00:00", "Thursday", False),
        ("europe/WARSAW", NOW, "2026-10-01T12:57:59+02:00", "Thursday", True),
        ("Asia/Kathmandu", NOW, "2026-10-01T16:42:59+05:45", "Thursday", False),
        ("Pacific/Kiritimati", NOW, "2026-10-02T00:57:59+14:00", "Friday", False),
        ("Pacific/Pago_Pago", NOW, "2026-09-30T23:57:59-11:00", "Wednesday", False),
        ("Australia/Lord_Howe", 1768000000, "2026-01-10T10:06:40+11:00", "Saturday", True),
        ("Australia/Lord_Howe", 1783000000, "2026-07-03T00:16:40+10:30", "Friday", False),
        ("Etc/GMT+5", NOW, "2026-10-01T05:57:59-05:00", "Thursday", False),
        ("America/New_York", 0, "1969-12-31T19:00:00-05:00", "Wednesday", False),
        ("America/New_York", -1, "1969-12-31T18:59:59-05:00", "Wednesday", False),
        ("Europe/Amsterdam", -1000000000, "1938-04-24T23:13:20+01:00", "Sunday", True),
        ("Europe/Paris", 4102444800, "2100-01-01T01:00:00+01:00", "Friday", False),
        ("Europe/Paris", 4118000000, "2100-06-30T02:53:20+02:00", "Wednesday", True),
        ("America/Sao_Paulo", 32503680000, "2999-12-31T21:00:00-03:00", "Tuesday", False),
    ]
    for zone, now, when, weekday, dst in cases:
        result = call_one("get_current_time", {"timezone": zone}, now=now)
        got = result.get("structuredContent")
        check(got == {"timezone": zone, "datetime": when, "day_of_week": weekday, "is_dst": dst},
              "get_current_time " + zone, str(got))


def test_convert_time():
    def convert(source, time, target, now=NOW):
        result = call_one("convert_time", {"source_timezone": source, "time": time, "target_timezone": target}, now=now)
        check(result["isError"] is False, "convert_time succeeds", str(result))
        check(result["content"][0]["text"] == json.dumps(result["structuredContent"], indent=2),
              "convert_time text is the result as indented JSON")
        return result["structuredContent"]

    got = convert("America/New_York", "16:30", "Asia/Kathmandu")
    check(list(got) == ["source", "target", "time_difference"], "convert_time key order")
    check(got == {
        "source": {"timezone": "America/New_York", "datetime": "2026-10-01T16:30:00-04:00", "day_of_week": "Thursday",
                   "is_dst": True},
        "target": {"timezone": "Asia/Kathmandu", "datetime": "2026-10-02T02:15:00+05:45", "day_of_week": "Friday",
                   "is_dst": False},
        "time_difference": "+9.75h"}, "convert_time New York to Kathmandu", str(got))

    differences = [("UTC", "UTC", "+0.0h"), ("Asia/Tokyo", "America/Los_Angeles", "-16.0h"),
                   ("UTC", "Asia/Kolkata", "+5.5h"), ("Asia/Kolkata", "UTC", "-5.5h"),
                   ("Asia/Kathmandu", "UTC", "-5.75h"), ("America/St_Johns", "Asia/Kathmandu", "+8.25h"),
                   ("Pacific/Pago_Pago", "Pacific/Kiritimati", "+25.0h")]
    for source, target, expected in differences:
        got = convert(source, "12:00", target)
        check(got["time_difference"] == expected, "time_difference %s to %s" % (source, target),
              got["time_difference"])

    # The date is today's date in the source zone, not in UTC: at 23:50 UTC
    # on September 30 it is already October 1 in London.
    got = convert("Europe/London", "09:00", "UTC", now=1790812200)
    check(got["source"]["datetime"] == "2026-10-01T09:00:00+01:00", "today in the source zone",
          got["source"]["datetime"])

    for time, expected in [("0:0", "T00:00:00"), ("7:05", "T07:05:00"), ("07:5", "T07:05:00"), ("23:59", "T23:59:00")]:
        got = convert("UTC", time, "UTC")
        check(expected in got["source"]["datetime"], "time " + time, got["source"]["datetime"])

    # A reading the clocks skip keeps the offset in effect before the change.
    got = convert("America/New_York", "02:30", "UTC", now=SPRING_FORWARD)
    check(got["source"] == {"timezone": "America/New_York", "datetime": "2026-03-08T02:30:00-05:00",
                            "day_of_week": "Sunday", "is_dst": False}, "skipped time, source", str(got["source"]))
    check(got["target"]["datetime"] == "2026-03-08T07:30:00+00:00", "skipped time, target", str(got["target"]))
    got = convert("America/New_York", "03:00", "UTC", now=SPRING_FORWARD)
    check(got["source"]["datetime"] == "2026-03-08T03:00:00-04:00" and got["source"]["is_dst"] is True,
          "first time after the skip")
    got = convert("America/New_York", "01:59", "UTC", now=SPRING_FORWARD)
    check(got["source"]["datetime"] == "2026-03-08T01:59:00-05:00", "last time before the skip")
    # A reading the clocks show twice is the first of the two.
    got = convert("America/New_York", "01:30", "UTC", now=FALL_BACK)
    check(got["source"]["datetime"] == "2026-11-01T01:30:00-04:00" and got["source"]["is_dst"] is True,
          "repeated time, source", str(got["source"]))
    check(got["target"]["datetime"] == "2026-11-01T05:30:00+00:00", "repeated time, target")
    got = convert("America/New_York", "02:00", "UTC", now=FALL_BACK)
    check(got["source"]["datetime"] == "2026-11-01T02:00:00-05:00" and got["source"]["is_dst"] is False,
          "first time after the repeat")
    # The change can also fall in the target zone.
    got = convert("UTC", "06:59", "America/New_York", now=SPRING_FORWARD)
    check(got["target"]["datetime"] == "2026-03-08T01:59:00-05:00", "target before its change")
    got = convert("UTC", "07:00", "America/New_York", now=SPRING_FORWARD)
    check(got["target"]["datetime"] == "2026-03-08T03:00:00-04:00", "target after its change")


def test_tool_errors():
    def error_text(name, arguments, label):
        result = call_one(name, arguments)
        check(result["isError"] is True, label + ": isError", str(result))
        check("structuredContent" not in result, label + ": no structuredContent")
        check(len(result["content"]) == 1 and result["content"][0]["type"] == "text", label + ": one text block")
        return result["content"][0]["text"]

    check(error_text("get_current_time", {}, "missing") == "Missing required argument: timezone", "missing argument")
    check(error_text("get_current_time", {"timezone": ""}, "empty") == "Missing required argument: timezone",
          "empty argument")
    for value in [5, 1.5, True, None, ["UTC"], {"a": 1}]:
        text = error_text("get_current_time", {"timezone": value}, "non-string")
        check(text == "Argument timezone must be a string", "non-string argument", text)
    replies = exchange([request(1, "tools/call", {"name": "get_current_time"})])
    check(replies[0]["result"]["isError"] is True, "no arguments at all")

    secret = open("/etc/passwd").read()[:20]
    bad_zones = ["Foo/Bar", "../etc/passwd", "/etc/passwd", "Etc/../UTC", "a//b", "a/./b", "UTC/", "/UTC", ".", "..",
                 "zone.tab", "+VERSION", "America", "America/", "Europe/Warsaw\u0000x", "Europe/War saw", "UTC\n",
                 "x" * 5000, "\u00e9", "America\\New_York", "America/New_York/x", "GMT+2", "PST",
                 # Long enough to be cut short when quoted back, with the cut inside a character.
                 "a" + "\u00e9" * 100, "ab" + "\u20ac" * 100, "\U0001f552" * 50 + "abc"]
    for zone in bad_zones:
        text = error_text("get_current_time", {"timezone": zone}, "bad zone " + repr(zone[:20]))
        check(text.startswith("Unknown timezone '") and "IANA" in text, "unknown zone text", text[:80])
        check(secret not in text and len(text) < 300, "error text is short and leaks nothing")

    args = {"source_timezone": "UTC", "time": "12:00", "target_timezone": "UTC"}
    for key in args:
        partial = dict(args)
        del partial[key]
        check(error_text("convert_time", partial, "missing " + key) == "Missing required argument: " + key,
              "missing " + key)
    check("'Bad/Source'" in error_text("convert_time", dict(args, source_timezone="Bad/Source"), "bad source"),
          "bad source zone named")
    check("'Bad/Target'" in error_text("convert_time", dict(args, target_timezone="Bad/Target"), "bad target"),
          "bad target zone named")
    for time in ["24:00", "12:60", "09:05 ", " 09:05", "\u0661\u0662:\u0660\u0665", "1205", "-1:00", "12:00:00",
                 "noon", ":30", "12:", "123:00", "12:000", "1e1:00", "+1:00", "12.00", "a" + "\u00e9" * 100]:
        text = error_text("convert_time", dict(args, time=time), "bad time " + repr(time))
        check(text.startswith("Invalid time '") and "HH:MM" in text, "bad time text", text)

    # An unknown tool and a malformed call are protocol errors.
    replies = exchange([call(1, "nope", {}), request(2, "tools/call", {"arguments": {}}),
                        request(3, "tools/call", {"name": 5}),
                        request(4, "tools/call", {"name": "get_current_time", "arguments": ["UTC"]}),
                        request(5, "tools/call")])
    check(replies[0]["error"] == {"code": -32602, "message": "Unknown tool: nope"}, "unknown tool", str(replies[0]))
    for reply in replies[1:]:
        check(reply["error"]["code"] == -32602, "malformed tools/call is -32602", str(reply))

    # A name is compared whole, including what follows a NUL character.
    replies = exchange([call(1, "get_current_time\u0000x", {"timezone": "UTC"}), request(2, "ping\u0000x"),
                        request(3, "tools/list", meta={PV: MODERN + "\u0000x", CAPS: {}})])
    check(replies[0].get("error", {}).get("code") == -32602, "tool name with a NUL is unknown", str(replies[0]))
    check(replies[1].get("error", {}).get("code") == -32601, "method with a NUL is unknown", str(replies[1]))
    check(replies[2].get("error", {}).get("code") == -32022 and
          replies[2]["error"]["data"]["requested"] == MODERN + "\u0000x",
          "protocol version with a NUL is unsupported", str(replies[2]))


def test_modern():
    replies = exchange([
        request("d", "server/discover", meta=MODERN_META),
        request(1, "tools/list", meta=MODERN_META),
        call(2, "get_current_time", {"timezone": "America/New_York"}, MODERN_META),
        call(3, "get_current_time", {"timezone": "Foo/Bar"}, MODERN_META),
        request(4, "ping", meta=MODERN_META),
        request(5, "tools/list", meta={PV: "2030-01-01", CAPS: {}}),
        request(6, "tools/list", meta={PV: MODERN}),
        request(7, "server/discover"),
        request(8, "tools/list", meta={PV: 5, CAPS: {}}),
        call(9, "nope", {}, MODERN_META),
        request(10, "server/discover", meta={PV: "1900-01-01", CAPS: {}}),
        request(11, "tools/list", meta=dict(MODERN_META, **{"io.modelcontextprotocol/clientInfo":
                                                           {"name": "c", "version": "1"}, "traceparent": "00-0-0-01"})),
    ])
    server_info = {"name": "time-mcp", "version": replies[0]["result"]["_meta"][INFO]["version"]}

    discover = replies[0]["result"]
    check(replies[0]["id"] == "d", "discover id")
    check(discover["resultType"] == "complete", "discover resultType")
    check(discover["supportedVersions"] == [MODERN] + LEGACY, "discover supportedVersions")
    check(discover["capabilities"] == {"tools": {"listChanged": False}}, "discover capabilities")
    check(discover["_meta"] == {INFO: server_info}, "discover serverInfo")
    check(isinstance(discover["ttlMs"], int) and discover["ttlMs"] >= 0 and discover["cacheScope"] == "public",
          "discover caching hints")

    listing = replies[1]["result"]
    check(listing["resultType"] == "complete", "tools/list resultType")
    check(isinstance(listing["ttlMs"], int) and listing["ttlMs"] >= 0 and listing["cacheScope"] == "public",
          "tools/list caching hints")
    check(listing["_meta"] == {INFO: server_info}, "tools/list serverInfo")
    check("nextCursor" not in listing, "one page")
    check_tools(listing["tools"], True, "modern")

    result = replies[2]["result"]
    check(result["resultType"] == "complete" and result["isError"] is False, "tools/call resultType")
    check(result["structuredContent"]["datetime"] == "2026-10-01T06:57:59-04:00", "tools/call structuredContent")
    check(result["content"][0]["text"] == json.dumps(result["structuredContent"], indent=2), "tools/call text")
    check(result["_meta"] == {INFO: server_info}, "tools/call serverInfo")
    check("ttlMs" not in result, "tool results carry no caching hints")

    result = replies[3]["result"]
    check(result["resultType"] == "complete" and result["isError"] is True and "structuredContent" not in result,
          "a failed call is a complete result with isError")

    check(replies[4]["error"]["code"] == -32601, "ping is gone in the modern revision")
    error = replies[5]["error"]
    check(error["code"] == -32022 and error["data"] == {"supported": [MODERN] + LEGACY, "requested": "2030-01-01"},
          "unsupported version is -32022 with the supported list", str(error))
    check(replies[6]["error"]["code"] == -32602, "missing clientCapabilities is -32602")
    check(replies[7]["error"]["code"] == -32602, "discover without a version is -32602")
    check(replies[8]["error"]["code"] == -32602, "non-string version is -32602")
    check(replies[9]["error"] == {"code": -32602, "message": "Unknown tool: nope"}, "unknown tool")
    check(replies[10]["error"]["code"] == -32022 and replies[10]["error"]["data"]["requested"] == "1900-01-01",
          "discover with an unsupported version is -32022")
    check(replies[11]["result"]["tools"] == listing["tools"], "extra _meta keys are accepted")
    check(len(replies) == 12, "modern reply count")


def test_both_eras():
    # One process serves a legacy session and modern requests side by side;
    # a modern request does not disturb the revision the handshake agreed.
    replies = exchange([
        request(1, "tools/list", meta=MODERN_META),
        initialize("2024-11-05", 2),
        request(3, "tools/list"),
        request(4, "tools/list", meta=MODERN_META),
        call(5, "get_current_time", {"timezone": "UTC"}),
        call(6, "get_current_time", {"timezone": "UTC"}, MODERN_META),
        request(7, "ping"),
        # A request may also name a legacy revision itself.
        call(8, "get_current_time", {"timezone": "UTC"}, {PV: "2025-06-18"}),
    ])
    check("resultType" in replies[0]["result"], "modern before the handshake")
    check(replies[1]["result"]["protocolVersion"] == "2024-11-05", "handshake")
    check("resultType" not in replies[2]["result"] and "outputSchema" not in replies[2]["result"]["tools"][0],
          "legacy list after the handshake")
    check("resultType" in replies[3]["result"] and "outputSchema" in replies[3]["result"]["tools"][0],
          "modern list after the handshake")
    check("structuredContent" not in replies[4]["result"] and "resultType" not in replies[4]["result"],
          "legacy call follows the agreed revision")
    check("structuredContent" in replies[5]["result"] and replies[5]["result"]["resultType"] == "complete",
          "modern call")
    check(replies[6]["result"] == {}, "legacy ping")
    check("structuredContent" in replies[7]["result"] and "resultType" not in replies[7]["result"],
          "a legacy revision named on the request")


def main():
    if not os.path.isfile(BINARY):
        print("no binary at %s" % BINARY)
        return 2
    for test in [test_command_line, test_legacy_handshake, test_base_protocol, test_transport, test_tools_list,
                 test_get_current_time, test_convert_time, test_tool_errors, test_modern, test_both_eras]:
        before = len(failures)
        test()
        print("%s %s" % ("ok  " if len(failures) == before else "FAIL", test.__name__))
    print("%d checks, %d failed" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
