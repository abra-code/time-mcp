# time-mcp

A small native Model Context Protocol (MCP) server that tells a language model the current time and converts times between time zones.

- One self-contained program, written in C++ with [yyjson](https://github.com/ibireme/yyjson) for JSON. No runtime, no interpreter, no MCP SDK, no other dependencies.
- Starts and answers in about 3 milliseconds.
- Uses no network and writes no files. It reads the clock and the system's time zone database, nothing else.
- Speaks MCP over standard input and output (stdio), in both protocol styles: the stateless revision 2026-07-28 and the older handshake revisions 2025-11-25, 2025-06-18, 2025-03-26 and 2024-11-05.
- macOS and Linux.

## Tools

Both tools are read-only and say so in their annotations (`readOnlyHint: true`), so a client has no reason to ask for permission before running them.

### get_current_time

| Argument | Type | |
|---|---|---|
| `timezone` | string, required | IANA time zone name, such as `America/New_York` or `Europe/London` |

```json
{
  "timezone": "America/New_York",
  "datetime": "2026-10-01T06:57:59-04:00",
  "day_of_week": "Thursday",
  "is_dst": true
}
```

### convert_time

| Argument | Type | |
|---|---|---|
| `source_timezone` | string, required | IANA time zone name the time is given in |
| `time` | string, required | 24-hour time, `HH:MM` |
| `target_timezone` | string, required | IANA time zone name to convert to |

The time is taken on today's date in the source time zone.

```json
{
  "source": {
    "timezone": "America/New_York",
    "datetime": "2026-10-01T16:30:00-04:00",
    "day_of_week": "Thursday",
    "is_dst": true
  },
  "target": {
    "timezone": "Asia/Kathmandu",
    "datetime": "2026-10-02T02:15:00+05:45",
    "day_of_week": "Friday",
    "is_dst": false
  },
  "time_difference": "+9.75h"
}
```

Each result is returned twice: as `structuredContent` (with an `outputSchema` in the tool list) for clients on revision 2025-06-18 or later, and as the same JSON in a text block for every client.

The tool names, arguments and result fields are those of the reference Python server `mcp-server-time`, so it can be replaced without changing prompts.

### Details

- Time zone names are matched without regard to case (`utc`, `europe/warsaw`), and the result repeats the name as it was given.
- `is_dst` is the daylight saving flag the zone database records for that moment.
- A time that the clocks skip on the day they go forward (02:30 in New York on the second Sunday of March) is read with the offset in effect before the change. A time that occurs twice on the day they go back is the first of the two.
- A wrong argument (unknown time zone, malformed time) comes back as a tool result with `isError: true` and a sentence the model can act on. An unknown tool name is a protocol error (`-32602`).

## Running

```
time-mcp [--local-timezone <name>]
time-mcp --version
time-mcp --help
```

`--local-timezone` names the time zone the tool descriptions suggest when the user mentions none. Without it the server uses the machine's own time zone (`TZ`, then `/etc/localtime`, then `/etc/timezone`, then `UTC`). A name that is not in the zone database is a startup error (exit status 2).

Example client configuration:

```json
{
  "mcpServers": {
    "time": {
      "command": "/usr/local/bin/time-mcp"
    }
  }
}
```

### Time zone database

The server reads the compiled zone files (TZif, RFC 9636) from `/usr/share/zoneinfo`, which macOS and Linux both provide, and falls back to `/usr/lib/zoneinfo`, `/usr/share/lib/zoneinfo` and `/etc/zoneinfo`. Set `TZDIR` to use another directory. On a minimal Linux image, install the `tzdata` package.

Time zone rules therefore follow the operating system's updates; nothing is compiled in.

## Building

Requires CMake 3.16 or later and a C++17 compiler.

```
cmake -S . -B build
cmake --build build
```

The result is `build/time-mcp`. On macOS, add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` to the first command for a universal binary.

## Testing

The tests need Python 3.9 or later (standard library only).

```
cmake --build build --target time-mcp zonedump
python3 test/test_time_mcp.py build/time-mcp
python3 test/test_tzif.py build/zonedump build/time-mcp
python3 test/test_zones.py build/zonedump build/time-mcp
```

- `test_time_mcp.py` drives the server over stdio: both protocol styles, every revision, the tool list, results, error cases and malformed input.
- `test_tzif.py` runs the time zone reader on zone files written for the test: every form of the rule that follows a zone's last recorded change, and files damaged in each way the reader must refuse.
- `test_zones.py` compares the time zone reader with two independent readers of the same database, Python's `zoneinfo` and the C library, for every zone: hourly through one year, coarsely from 1900 to 2100, and every 5 minutes around each clock change. It takes about a minute.

Or, after building both targets, `ctest --test-dir build`.

## Protocol notes

- Messages are newline-delimited JSON-RPC 2.0, one per line. Lines longer than 1 MB are refused.
- A request that carries `io.modelcontextprotocol/protocolVersion` in `params._meta` is served statelessly under revision 2026-07-28 (`server/discover`, `resultType`, `ttlMs` and `cacheScope` on list results, server identity in each result's `_meta`). A client that opens with `initialize` gets the handshake revisions. One process serves both.
- JSON-RPC batches of up to 100 messages are accepted, as revision 2025-03-26 requires.
- The server exits with status 0 when its input ends, after answering every complete line.
