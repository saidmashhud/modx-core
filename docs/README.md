# modx-core documentation

modx-core is a small backend framework in C11: an HTTP/1.1 server on an event loop with Server-Sent
Events and WebSocket, a durable in-memory document store (snapshot + journal) with a change feed, and the
pieces a web application needs around them (JSON, crypto and password hashing on a worker pool, sessions,
idempotency keys, operational transformation). libc / POSIX threads plus the system zlib; nothing else.

## Start here

- [Getting started](getting-started.md): build, test, install, link with pkg-config or CMake, and a
  minimal server (`examples/minimal/minimal.c`) explained line by line.

## Guides

| Guide | Covers |
|---|---|
| [HTTP and routing](guides/http.md) | the event loop, handlers and responses, routing, limits and timeouts, static files, gzip, IPv4 / IPv6 / dual stack, deferred responses, the durability barrier, graceful shutdown |
| [Store and durability](guides/store.md) | collections, mutations, group commit, the change ring and catch-up, compaction, the read-only state after an I/O failure, recovery and salvage mode, the on-disk format |
| [Sessions and passwords](guides/sessions.md) | cookies, CSRF, client addresses behind a proxy, IPv6 limiter keys, failure throttling, session tokens, password hashing (600 000 PBKDF2 iterations) on the worker pool, rehash on login |
| [Server-Sent Events](guides/sse.md) | streams, broadcast with filters, catch-up with `Last-Event-ID`, pings |
| [WebSocket](guides/websocket.md) | the handshake, messages, close codes, limits, pings |
| [Idempotency keys](guides/idempotency.md) | `Idempotency-Key`: validation, remembered answers, replay, retention |
| [Operational transformation](guides/ot.md) | plain-text OT compatible with ot.js (UTF-16 offsets) |

## Reference

- [API reference (summary per header)](reference.md).
- The full reference is generated from the header comments: `make docs` (Doxygen; writes
  `build/docs/html/index.html`; skipped with a note when doxygen is not installed).
- [CHANGELOG](../CHANGELOG.md), and the [README](../README.md) for the guarantees, the measured
  performance table and the limits.

## Conventions used throughout

- **One loop thread.** The server, the store, the SSE hub and every callback the framework makes (handlers,
  ticks, listeners, pool completions) run on the thread that calls `http_server_run()`. The only other
  threads are the worker pool's (`modx/pool.h`), which run self-contained jobs and never touch the server or
  the store.
- **Ownership is spelled out per function.** "Takes ownership" means the callee frees it (even on failure,
  where stated); "borrowed" pointers stay valid until the next mutation of what they point into; functions
  that return `char *` / arrays say who frees them.
- **Errors.** Functions return 0 / -1 (with `errno` where the comment says so), or NULL; open and parse
  functions fill a caller-provided `err` buffer with a message meant for logs.
