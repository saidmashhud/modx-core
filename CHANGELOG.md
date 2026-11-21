# Changelog

## 0.1.0 — 2026-11-21

First public version, extracted from a production server and then optimized step by step (each step is
one commit with measured numbers; see the README table).

- HTTP/1.1 server: keep-alive, pipelining, `Expect: 100-continue`, per-route body limits, streamed file
  responses, static files with SPA fallback and weak ETag / 304, gzip through zlib.
- Event loop on kqueue (macOS / BSD) and epoll (Linux), poll() fallback; configurable connection limit.
- Store: application-defined collections, snapshot + CRC journal with torn-tail recovery, change ring with
  catch-up, hooks for keys / redaction / scopes / derived state, secondary indexes.
- Group commit: one `write()` + one `fsync()` per loop iteration; responses and pushes only after it.
- Background compaction: fork + copy-on-write snapshot, journal rotation, crash-safe at every step.
- Stability: header / body / write / idle timeouts, lingering close, graceful drain on SIGTERM.
- SSE hub and WebSocket (RFC 6455) on the same loop.
- JSON, SHA-256 / HMAC / PBKDF2 / SHA-1 / base64, cookie-session helpers, response cache, OT, timers.
- Tests: unit tests, an end-to-end test of the example (both pollers), crash-point tests of the journal and
  snapshot, ASan + UBSan, mutation fuzzing and libFuzzer targets (HTTP, JSON, journal, WebSocket).
- `bench/`: load generator, driver and a script that benchmarks several revisions back to back.
