# Changelog

## 0.2.0 — Unreleased

Hardening.

- Store: **a failed journal write or fsync is sticky.** A `write()` error used to drop the queued records
  while they stayed in memory with their seq; the next sync's fsync then succeeded and announced them
  durable (listener / SSE), and a restart lost them. A failed `fsync()` was retried, which proves nothing
  once one failed (fsyncgate). Now the store becomes read-only until reopened: nothing is announced durable
  again, fsync is not retried, mutations return -1 (`EROFS`), compaction is refused, reads keep working.
  `mx_store_sync` returns -1 for the failing batch and `MX_STORE_EFAILED` (-2) after it; new
  `mx_store_failed()`, `mx_store_health_get()` / `mx_store_health`; new fields on `mx_store`.
- HTTP: a `durable_commit` result of `HTTP_DURABLE_UNAVAILABLE` (-2) or less turns the held responses
  into 503 (500 for -1, as before).
- HTTP: request smuggling hardening. Field names must be RFC 9110 tokens (`Content-Length : 5`, obs-fold
  lines → 400 instead of an ignored header); a bare CR or LF anywhere in the header block → 400; control
  characters in the request-target or (except HTAB) in field values → 400.
- HTTP: an HTTP/1.1 request without `Host`, or any request with two, → 400 (RFC 9112 §3.2).
- HTTP: socket writes use `send(..., MSG_NOSIGNAL)` where available, so a closed peer cannot raise
  SIGPIPE on Linux (`SO_NOSIGPIPE` stays on macOS / BSD).
- Sessions: the failure rate limiter is bounded (`MX_RL_CAPACITY` = 4096 keys, or `rl_init_cap`) with a
  hash index; when full it drops aged-out keys, then evicts the oldest unblocked one. `rl_reset` removes
  the key. `ratelimit` / `rl_entry` gained fields.
- `modx/version.h`: `MODX_VERSION_MAJOR` / `MINOR` / `PATCH`, `MODX_VERSION`, `MODX_VERSION_NUM` and
  `modx_version()` (new source file `src/version.c`).
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
