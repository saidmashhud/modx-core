# Changelog

## Unreleased

- `modx/idem.h`: idempotency keys. Key validation (1–128 printable ASCII), the methods a key applies to
  (POST / PUT / PATCH / DELETE), which answers are remembered (2xx and 4xx except 408 / 429; never 5xx, SSE,
  WebSocket or file bodies), records (method, target, status, content type, body — or a `body_omitted` marker
  for large / non-text bodies), method + target matching, replay with `Idempotent-Replayed: true`, and
  retention helpers (`mx_idem_prune` by age, `mx_idem_cap` per owner). The application keeps the records in a
  private no-ring collection of its store and chooses the key scope.
- Store: a journal record of a collection this build does not know (written by a newer version) is skipped
  with its seq kept (`skipped_records`), instead of being treated as a corrupt tail — rolling back a release
  no longer cuts the journal at the first record of a new collection.

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
