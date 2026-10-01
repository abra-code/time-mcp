# time-mcp

A small native Model Context Protocol (MCP) server that tells a language model the current time and converts times between time zones.

- One self-contained program, written in C++ with [yyjson](https://github.com/ibireme/yyjson) for JSON. No runtime, no interpreter, no MCP SDK, no other dependencies.
- Starts and answers in about 3 milliseconds.
- Uses no network and writes no files. It reads the clock and the system's time zone database, nothing else.
- Speaks MCP over standard input and output (stdio), in both protocol styles: the stateless revision 2026-07-28 and the older handshake revisions 2025-11-25, 2025-06-18, 2025-03-26 and 2024-11-05.
- macOS and Linux.

## Protocol notes

- Messages are newline-delimited JSON-RPC 2.0, one per line. Lines longer than 1 MB are refused.
- A request that carries `io.modelcontextprotocol/protocolVersion` in `params._meta` is served statelessly under revision 2026-07-28 (`server/discover`, `resultType`, `ttlMs` and `cacheScope` on list results, server identity in each result's `_meta`). A client that opens with `initialize` gets the handshake revisions. One process serves both.
- JSON-RPC batches of up to 100 messages are accepted, as revision 2025-03-26 requires.
- The server exits with status 0 when its input ends, after answering every complete line.
