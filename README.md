# time-mcp

A small native Model Context Protocol (MCP) server that tells a language model the current time and converts times between time zones.

- One self-contained program, written in C++ with [yyjson](https://github.com/ibireme/yyjson) for JSON. No runtime, no interpreter, no MCP SDK, no other dependencies.
- Starts and answers in about 3 milliseconds.
- Uses no network and writes no files. It reads the clock and the system's time zone database, nothing else.
- Speaks MCP over standard input and output (stdio), in both protocol styles: the stateless revision 2026-07-28 and the older handshake revisions 2025-11-25, 2025-06-18, 2025-03-26 and 2024-11-05.
- macOS and Linux.

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
python3 test/test_zones.py build/zonedump
```

- `test_zones.py` compares the time zone reader with two independent readers of the same database, Python's `zoneinfo` and the C library, for every zone: hourly through one year, coarsely from 1900 to 2100, and every 5 minutes around each clock change. It takes about a minute.

Or, after building both targets, `ctest --test-dir build`.

## Protocol notes

- Messages are newline-delimited JSON-RPC 2.0, one per line. Lines longer than 1 MB are refused.
- A request that carries `io.modelcontextprotocol/protocolVersion` in `params._meta` is served statelessly under revision 2026-07-28 (`server/discover`, `resultType`, `ttlMs` and `cacheScope` on list results, server identity in each result's `_meta`). A client that opens with `initialize` gets the handshake revisions. One process serves both.
- JSON-RPC batches of up to 100 messages are accepted, as revision 2025-03-26 requires.
- The server exits with status 0 when its input ends, after answering every complete line.
