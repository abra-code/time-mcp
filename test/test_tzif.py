#!/usr/bin/env python3
"""Checks time-mcp's time zone reader on zone files made for the purpose:
each form of the rule that follows the last recorded transition, and files
that are damaged in every way the reader has to notice.

Usage: python3 test_tzif.py [path/to/zonedump] [path/to/time-mcp]

The files are written to a temporary directory that the programs are
pointed at with TZDIR. Expected instants are computed here from calendar
dates, not taken from another time zone library.
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
ZONEDUMP = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "zonedump")
BINARY = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(HERE, "..", "build", "time-mcp")

HOUR = 3600

failures = []
checks = 0


def check(condition, label, detail=""):
    global checks
    checks += 1
    if not condition:
        failures.append(label)
        print("FAIL: %s %s" % (label, detail))


def utc(year, month, day, hour=0, minute=0):
    return int(datetime(year, month, day, hour, minute, tzinfo=timezone.utc).timestamp())


def block(version, transitions, types, time_format):
    """One header and data block. `transitions` is [(instant, type index)],
    `types` is [(offset, is_dst)]."""
    data = b"TZif" + version + b"\0" * 15 + struct.pack(">6l", 0, 0, 0, len(transitions), len(types), 4)
    data += b"".join(struct.pack(time_format, instant) for instant, _ in transitions)
    data += bytes(index for _, index in transitions)
    data += b"".join(struct.pack(">lBB", offset, dst, 0) for offset, dst in types)
    return data + b"LMT\0"


def tzif(rule=b"", transitions=(), types=((0, 0),), version=b"2"):
    """A whole zone file. Version 1 has one block with 32-bit instants;
    later versions add a block with 64-bit instants and the rule footer."""
    if version == b"\0":
        return block(version, transitions, types, ">l")
    return block(version, (), ((0, 0),), ">l") + block(version, transitions, types, ">q") + b"\n" + rule + b"\n"


class Dump:
    """A running zonedump process reading zones from `directory`."""

    def __init__(self, directory):
        env = dict(os.environ, TZDIR=directory)
        self.process = subprocess.Popen([ZONEDUMP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env,
                                        universal_newlines=True)

    def ask(self, command, zone, start, end, step):
        self.process.stdin.write("%s %s %d %d %d\n" % (command, zone, start, end, step))
        self.process.stdin.flush()
        lines = []
        while True:
            line = self.process.stdout.readline().rstrip("\n")
            if not line:
                raise RuntimeError("zonedump stopped")
            if line.startswith("error"):
                return line
            if line == "end":
                return [tuple(int(field) for field in entry.split()) for entry in lines]
            lines.append(line)

    def close(self):
        self.process.stdin.close()
        self.process.wait()


def write(directory, name, data):
    path = os.path.join(directory, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as file:
        file.write(data)


def test_rules(directory, dump):
    def changes(name, rule, start, end, step=HOUR, command="utc"):
        write(directory, name, tzif(rule))
        return dump.ask(command, name, start, end, step)

    # No daylight saving time at all.
    got = changes("Fixed", b"AAA-5:30", utc(2026, 1, 1), utc(2027, 1, 1))
    check(got == [(utc(2026, 1, 1), 19800, 0)], "fixed offset", str(got))
    got = changes("FixedQuoted", b"<+0545>-5:45", utc(2026, 1, 1), utc(2027, 1, 1))
    check(got == [(utc(2026, 1, 1), 20700, 0)], "fixed offset, quoted name", str(got))
    got = changes("FixedWest", b"AAA3:30:15", utc(2026, 1, 1), utc(2027, 1, 1))
    check(got == [(utc(2026, 1, 1), -12615, 0)], "fixed offset west with seconds", str(got))

    # Last Sunday of March at 02:00 standard time to last Sunday of October
    # at 03:00 daylight time, one hour east: 2026-03-29 and 2026-10-25,
    # both at 01:00 UTC.
    got = changes("North", b"AAA-1BBB,M3.5.0,M10.5.0/3", utc(2026, 1, 1), utc(2027, 1, 1))
    check(got == [(utc(2026, 1, 1), 3600, 0), (utc(2026, 3, 29, 1), 7200, 1), (utc(2026, 10, 25, 1), 3600, 0)],
          "northern rule", str(got))
    # First Sunday (week 1) and a weekday other than Sunday: 2026-04-05 is
    # the first Sunday of April, 2026-09-04 the first Friday of September.
    got = changes("FirstWeek", b"AAA0BBB,M4.1.0/0,M9.1.5/0", utc(2026, 1, 1), utc(2027, 1, 1))
    check(got == [(utc(2026, 1, 1), 0, 0), (utc(2026, 4, 5), 3600, 1), (utc(2026, 9, 3, 23), 0, 0)],
          "week 1 and a weekday", str(got))
    # A daylight offset other than one hour ahead.
    got = changes("HalfHour", b"AAA-10:30BBB-11,M10.1.0,M4.1.0", utc(2026, 6, 1), utc(2026, 6, 2))
    check(got == [(utc(2026, 6, 1), 37800, 0)], "explicit daylight offset, standard half", str(got))
    got = changes("HalfHour", b"AAA-10:30BBB-11,M10.1.0,M4.1.0", utc(2026, 12, 1), utc(2026, 12, 2))
    check(got == [(utc(2026, 12, 1), 39600, 1)], "explicit daylight offset, daylight half", str(got))

    # Southern hemisphere: daylight time spans the new year. First Sunday of
    # October 02:00 standard (+10) to first Sunday of April 03:00 daylight.
    got = changes("South", b"AAA-10BBB,M10.1.0,M4.1.0/3", utc(2026, 1, 1), utc(2027, 1, 1))
    check(got == [(utc(2026, 1, 1), 39600, 1), (utc(2026, 4, 4, 16), 36000, 0), (utc(2026, 10, 3, 16), 39600, 1)],
          "southern rule", str(got))

    # A negative time of day: the day before the last Sunday of March at
    # 23:00 (the rule three Greenland zones use).
    got = changes("Negative", b"<-02>2<-01>,M3.5.0/-1,M10.5.0/0", utc(2026, 1, 1), utc(2027, 1, 1))
    check(got == [(utc(2026, 1, 1), -7200, 0), (utc(2026, 3, 29, 1), -3600, 1), (utc(2026, 10, 25, 1), -7200, 0)],
          "negative time of day", str(got))
    # A time of day past 24 hours: two days and two hours after the fourth Thursday.
    got = changes("PastDay", b"AAA-2BBB,M3.4.4/50,M10.5.0/2", utc(2026, 3, 1), utc(2026, 4, 30))
    check(got == [(utc(2026, 3, 1), 7200, 0), (utc(2026, 3, 28, 0), 10800, 1)], "time of day past 24 hours", str(got))

    # A change that lands in the year before the one its rule is for:
    # January 1 minus one hour is December 31, 23:00.
    got = changes("YearEdge", b"AAA0BBB,0/-1,J100", utc(2025, 12, 31, 12), utc(2026, 12, 31, 22))
    check(got == [(utc(2025, 12, 31, 12), 0, 0), (utc(2025, 12, 31, 23), 3600, 1), (utc(2026, 4, 10, 1), 0, 0)],
          "change before the start of its year", str(got))
    # And one that lands in the year after: December 31 plus 30 hours.
    got = changes("YearEdgeLate", b"AAA0BBB,J200,J365/30", utc(2026, 1, 1), utc(2026, 12, 31, 23))
    check(got == [(utc(2026, 1, 1), 3600, 1), (utc(2026, 1, 1, 5), 0, 0), (utc(2026, 7, 19, 2), 3600, 1)],
          "change after the end of its year", str(got))

    # Both changes land in the year after: December 31 plus 100 hours ends
    # daylight time on January 4 at 04:00, plus 167 hours starts it again on
    # January 6 at 23:00. The first days of January are still daylight time
    # from the January 6 of the year before.
    got = changes("BothLate", b"AAA0BBB,J365/167,J365/100", utc(2025, 12, 20), utc(2026, 12, 31))
    check(got == [(utc(2025, 12, 20), 3600, 1), (utc(2026, 1, 4, 3), 0, 0), (utc(2026, 1, 6, 23), 3600, 1)],
          "both changes after the end of their year", str(got))
    got = changes("BothLate", b"AAA0BBB,J365/167,J365/100", utc(2026, 1, 1), utc(2026, 1, 10), 300, "wall")
    check(got == [(utc(2026, 1, 1), 3600, 1), (utc(2026, 1, 4, 4), 0, 0), (utc(2026, 1, 7), 3600, 1)],
          "wall readings where both changes are after the end of their year", str(got))

    # Daylight time all year: it starts on January 1 at 00:00 and ends at
    # the same instant a year later.
    got = changes("AllYear", b"AAA1BBB,0/0,J365/25", utc(2025, 12, 1), utc(2029, 2, 1))
    check(got == [(utc(2025, 12, 1), 0, 1)], "daylight time all year", str(got))

    # Day numbers. J counts 1-365 and never February 29, so J60 is March 1
    # in every year; a plain number counts from 0 and does count it, so 59
    # is February 29 in a leap year and March 1 otherwise.
    for year, plain in [(2027, (3, 1)), (2028, (2, 29))]:
        got = changes("Julian", b"AAA0BBB,J60/0,J300/0", utc(year, 1, 1), utc(year, 6, 1))
        check(got == [(utc(year, 1, 1), 0, 0), (utc(year, 3, 1), 3600, 1)], "J day in %d" % year, str(got))
        got = changes("Plain", b"AAA0BBB,59/0,300/0", utc(year, 1, 1), utc(year, 6, 1))
        check(got == [(utc(year, 1, 1), 0, 0), (utc(year, plain[0], plain[1]), 3600, 1)],
              "plain day in %d" % year, str(got))
    got = changes("Julian", b"AAA0BBB,J60/0,J300/0", utc(2028, 10, 1), utc(2028, 12, 1))
    check(got == [(utc(2028, 10, 1), 3600, 1), (utc(2028, 10, 26, 23), 0, 0)], "J300 in a leap year", str(got))

    # No dates: second Sunday of March to first Sunday of November, 02:00.
    got = changes("Default", b"EST5EDT", utc(2026, 1, 1), utc(2027, 1, 1))
    check(got == [(utc(2026, 1, 1), -18000, 0), (utc(2026, 3, 8, 7), -14400, 1), (utc(2026, 11, 1, 6), -18000, 0)],
          "rule without dates", str(got))

    # Wall clock readings: the new type applies once the clocks pass the
    # later reading of the change (03:00 local in both directions here).
    spring = utc(2026, 3, 29, 1)
    got = changes("North", b"AAA-1BBB,M3.5.0,M10.5.0/3", spring, spring + 4 * HOUR, 300, "wall")
    check(got == [(spring, 3600, 0), (spring + 2 * HOUR, 7200, 1)], "wall readings across the skipped hour", str(got))
    fall = utc(2026, 10, 25, 1)
    got = changes("North", b"AAA-1BBB,M3.5.0,M10.5.0/3", fall, fall + 4 * HOUR, 300, "wall")
    check(got == [(fall, 7200, 1), (fall + 2 * HOUR, 3600, 0)], "wall readings across the repeated hour", str(got))


def test_transitions(directory, dump):
    types = ((1800, 0), (7200, 1), (3600, 0))
    transitions = ((1000, 1), (2000, 2))
    expected = [(0, 1800, 0), (1000, 7200, 1), (2000, 3600, 0)]

    # Version 1: 32-bit instants, no rule. The last type holds for good.
    write(directory, "V1", tzif(transitions=transitions, types=types, version=b"\0"))
    got = dump.ask("utc", "V1", 0, 3000, 500)
    check(got == expected, "version 1 file", str(got))
    got = dump.ask("utc", "V1", utc(2100, 1, 1), utc(2100, 1, 2), HOUR)
    check(got == [(utc(2100, 1, 1), 3600, 0)], "version 1 file, far future", str(got))

    # Before the first transition the first standard type applies, even
    # when the first type listed is a daylight one.
    write(directory, "DstFirst", tzif(transitions=((1000, 0), (2000, 1)), types=((7200, 1), (3600, 0)),
                                      version=b"\0"))
    got = dump.ask("utc", "DstFirst", 0, 3000, 500)
    check(got == [(0, 3600, 0), (1000, 7200, 1), (2000, 3600, 0)], "before the first transition", str(got))

    # Versions 2 to 4: 64-bit instants, then the rule takes over.
    far = utc(2040, 6, 1)
    for version in (b"2", b"3", b"4"):
        write(directory, "V" + version.decode(), tzif(b"AAA-1BBB,M3.5.0,M10.5.0/3", ((1000, 1), (far, 2)), types,
                                                      version))
        got = dump.ask("utc", "V" + version.decode(), 0, 3000, 500)
        check(got == [(0, 1800, 0), (1000, 7200, 1)], "version %s file" % version.decode(), str(got))
        got = dump.ask("utc", "V" + version.decode(), far - HOUR, far + HOUR, HOUR)
        # From the last transition on the rule decides, not the type the
        # transition names: June is daylight time under this rule.
        check(got == [(far - HOUR, 7200, 1)], "the rule applies from the last transition", str(got))
        got = dump.ask("utc", "V" + version.decode(), utc(2041, 1, 1), utc(2042, 1, 1), HOUR)
        check(got == [(utc(2041, 1, 1), 3600, 0), (utc(2041, 3, 31, 1), 7200, 1), (utc(2041, 10, 27, 1), 3600, 0)],
              "version %s file, rule years" % version.decode(), str(got))

    # Instants beyond 32 bits, and an empty rule.
    write(directory, "Wide", tzif(b"", ((-(1 << 40), 1), (1 << 40, 2)), types))
    got = dump.ask("utc", "Wide", (1 << 40) - 1, (1 << 40) + 1, 1)
    check(got == [((1 << 40) - 1, 7200, 1), (1 << 40, 3600, 0)], "64-bit instants", str(got))
    got = dump.ask("utc", "Wide", -(1 << 40) - 1, -(1 << 40), 1)
    check(got == [(-(1 << 40) - 1, 1800, 0), (-(1 << 40), 7200, 1)], "64-bit instants before 1970", str(got))


def test_damaged(directory, dump):
    good = tzif(b"AAA-1BBB,M3.5.0,M10.5.0/3", ((1000, 1), (2000, 0)), ((3600, 0), (7200, 1)))
    write(directory, "Good", good)
    check(isinstance(dump.ask("utc", "Good", 0, 10, 1), list), "the undamaged file loads")

    second = good.index(b"TZif", 4)
    # Offsets of the counts in the second header, which the reader uses.
    time_count = second + 32
    type_count = second + 36
    first_index = second + 44 + 2 * 8

    def patched(position, replacement):
        return good[:position] + replacement + good[position + len(replacement):]

    def with_rule(rule):
        return tzif(rule, ((1000, 1), (2000, 0)), ((3600, 0), (7200, 1)))

    damaged = {
        "Empty": b"",
        "Short": good[:20],
        "Magic": b"TZIF" + good[4:],
        "SecondMagic": patched(second, b"XXXX"),
        "HeaderOnly": good[:44],
        "FirstBlockOnly": good[:second],
        "CutInSecondHeader": good[:second + 30],
        "CutInTransitions": good[:first_index - 5],
        "CutInTypes": good[:first_index + 5],
        "NoFooter": good[:good.rindex(b"\n", 0, len(good) - 1)],
        "FooterNotClosed": good[:-1],
        "FooterNotOpened": good.replace(b"\nAAA", b"AAAA"),
        "TypeIndex": patched(first_index, b"\x02"),
        "NoTypes": patched(type_count, struct.pack(">l", 0)),
        "ManyTypes": patched(type_count, struct.pack(">l", 300)),
        "HugeTimeCount": patched(time_count, struct.pack(">l", 0x7FFFFFFF)),
        "NegativeTimeCount": patched(time_count, struct.pack(">l", -1)),
        "HugeFirstCount": patched(32, struct.pack(">l", 0x7FFFFFFF)),
        "NotAscending": tzif(b"", ((2000, 1), (1000, 0)), ((3600, 0), (7200, 1))),
        "Repeated": tzif(b"", ((1000, 1), (1000, 0)), ((3600, 0), (7200, 1))),
        "OffsetTooLarge": tzif(b"", types=((200000, 0),)),
        "OffsetTooSmall": tzif(b"", types=((-200000, 0),)),
        "TooBig": good + b"\0" * (1 << 20),
        "RuleGarbage": with_rule(b"nonsense"),
        "RuleShortName": with_rule(b"AA0"),
        "RuleShortQuoted": with_rule(b"<A>0"),
        "RuleOpenQuote": with_rule(b"<AAA0"),
        "RuleQuoteCharacter": with_rule(b"<A,A>0"),
        "RuleNoOffset": with_rule(b"AAA"),
        "RuleOffsetRange": with_rule(b"AAA25"),
        "RuleMinuteRange": with_rule(b"AAA1:60"),
        "RuleMonth": with_rule(b"AAA0BBB,M13.1.0,M10.1.0"),
        "RuleWeek": with_rule(b"AAA0BBB,M3.6.0,M10.1.0"),
        "RuleWeekZero": with_rule(b"AAA0BBB,M3.0.0,M10.1.0"),
        "RuleWeekday": with_rule(b"AAA0BBB,M3.1.7,M10.1.0"),
        "RuleJulianZero": with_rule(b"AAA0BBB,J0,J100"),
        "RuleJulianRange": with_rule(b"AAA0BBB,J1,J366"),
        "RuleDayRange": with_rule(b"AAA0BBB,0,366"),
        "RuleOneDate": with_rule(b"AAA0BBB,M3.1.0"),
        "RuleTimeRange": with_rule(b"AAA0BBB,M3.1.0/168,M10.1.0"),
        "RuleTrailing": with_rule(b"AAA0BBB,M3.1.0,M10.1.0,"),
        "RuleTrailingText": with_rule(b"AAA0 "),
        "RuleNul": with_rule(b"AAA0\0BBB"),
    }
    for name, data in sorted(damaged.items()):
        write(directory, name, data)
        got = dump.ask("utc", name, 0, 10, 1)
        check(isinstance(got, str) and got.startswith("error"), "damaged file is refused: " + name, str(got)[:80])

    os.makedirs(os.path.join(directory, "Folder", "Inner"), exist_ok=True)
    got = dump.ask("utc", "Folder", 0, 10, 1)
    check(isinstance(got, str), "a directory is not a zone", str(got)[:80])
    got = dump.ask("utc", "Missing/Zone", 0, 10, 1)
    check(isinstance(got, str), "a missing file is not a zone", str(got)[:80])


def test_lookup(directory):
    """Through the server: names, links and case."""
    write(directory, "Area/Some_City", tzif(b"AAA-1"))
    write(directory, "UTC", tzif(b"UTC0"))
    os.symlink("Area/Some_City", os.path.join(directory, "Linked"))
    os.symlink("/etc/passwd", os.path.join(directory, "Passwd"))
    os.symlink(os.path.join(directory, "Nowhere"), os.path.join(directory, "Dangling"))

    names = ["Area/Some_City", "area/some_city", "AREA/SOME_CITY", "Linked", "linked", "UTC", "Area", "Area/",
             "Area/Other", "Passwd", "Dangling", "Good", "Magic", "RuleGarbage", "Folder", "folder/inner"]
    requests = [{"jsonrpc": "2.0", "id": index, "method": "tools/call",
                 "params": {"name": "get_current_time", "arguments": {"timezone": name}}}
                for index, name in enumerate(names)]
    env = dict(os.environ, TZDIR=directory, TIME_MCP_TEST_NOW="1790852279")
    env.pop("TZ", None)
    data = "".join(json.dumps(r) + "\n" for r in requests).encode()
    done = subprocess.run([BINARY, "--local-timezone", "UTC"], input=data, stdout=subprocess.PIPE, env=env, timeout=30)
    check(done.returncode == 0, "the server runs on the test directory", str(done.returncode))
    replies = [json.loads(line) for line in done.stdout.decode().split("\n")[:-1]]
    check(len(replies) == len(names), "one reply per request", str(len(replies)))
    loads = {"Area/Some_City": "+01:00", "area/some_city": "+01:00", "AREA/SOME_CITY": "+01:00", "Linked": "+01:00",
             "linked": "+01:00", "UTC": "+00:00", "Good": "+02:00"}
    for name, reply in zip(names, replies):
        result = reply["result"]
        if name in loads:
            check(result["isError"] is False and result["structuredContent"]["timezone"] == name and
                  result["structuredContent"]["datetime"].endswith(loads[name]), "zone loads: " + name, str(result))
        else:
            text = result["content"][0]["text"]
            check(result["isError"] is True and text.startswith("Unknown timezone '"), "zone refused: " + name,
                  str(result))
            check("root" not in text, "nothing of the file is quoted: " + name)

    # A local zone the directory does not have stops the server at startup.
    done = subprocess.run([BINARY, "--local-timezone", "Europe/Warsaw"], input=b"", stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env, timeout=30)
    check(done.returncode == 2 and directory.encode() in done.stderr, "startup error names the directory")
    done = subprocess.run([BINARY, "--local-timezone", "Magic"], input=b"", stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env, timeout=30)
    check(done.returncode == 2, "a damaged local zone stops the server")


def main():
    for path in (ZONEDUMP, BINARY):
        if not os.path.isfile(path):
            print("no binary at %s" % path)
            return 2
    directory = tempfile.mkdtemp(prefix="time-mcp-tzif-")
    try:
        dump = Dump(directory)
        for test in (test_rules, test_transitions, test_damaged):
            before = len(failures)
            test(directory, dump)
            print("%s %s" % ("ok  " if len(failures) == before else "FAIL", test.__name__))
        dump.close()
        before = len(failures)
        test_lookup(directory)
        print("%s %s" % ("ok  " if len(failures) == before else "FAIL", "test_lookup"))
    finally:
        shutil.rmtree(directory, ignore_errors=True)
    print("%d checks, %d failed" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
