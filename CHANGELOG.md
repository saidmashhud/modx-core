# Changelog

## 0.3.0 — 2026-11-25

- Store: **recovery no longer drops data silently.** Open used to stop replaying at the first bad record
  anywhere in the journal and cut everything after it, and did not notice seq gaps. Now only a torn tail
  (the bytes after the last record with a valid CRC in `journal.log`) is cut off and reported
  (`recovered_truncated`, new `recovered_tail_bytes`). A bad record followed by valid ones, a seq gap or a
  seq going backwards after the snapshot's seq, or any damage in `journal.old` (fsynced before rotation)
  fail the open with a message naming the file, byte offset and line. A CRC-valid record without id / op /
  data, or a put whose row has no string `"id"`, counts as damage too (such a row used to be stored, and the
  next snapshot crashed sorting it; the snapshot sort is now NULL-safe as well).
- Store: **salvage mode**: `mx_store_open_ex()` with `mx_store_opts { salvage, report_path }`, or the
  environment variable `MODX_STORE_SALVAGE=1`. Keeps every valid record before and after damaged ones,
  accepts seq gaps, skips records whose seq goes backwards, keeps the damaged files as
  `<file>.damaged-<unix ms>`, writes a report (`<dir>/recovery-<unix ms>.txt`) and compacts the recovered
  state into a fresh snapshot and an empty journal. New fields `salvage`, `salvaged_records`,
  `salvaged_bytes`, `seq_gaps`, `snapshot_crc_mismatch`, `recovery_report` on `mx_store`.
- **Password hashing no longer blocks the event loop.** New `modx/pool.h`: a small pthreads worker pool
  (`mx_pool_new`, `mx_pool_submit`, `mx_pool_attach`, `mx_pool_run_done`, `mx_pool_wait`, `mx_pool_free`)
  whose completions run on the loop thread; the server's poller is woken through a pipe. New
  `password_hash_async()` / `password_verify_async()` (callback on the loop thread; the password is copied
  and wiped; salts are drawn on the loop thread so workers take no locks and never malloc), and
  `password_needs_rehash()` for rehash-on-login. The sync `password_hash` / `password_verify` stay for tools
  and tests. Linking now needs `-pthread` (the Makefile and the pkg-config / CMake files pass it).
- **Default PBKDF2-SHA256 iterations: 600 000** (OWASP), up from 150 000 (`PASSWORD_ITERS`;
  `PASSWORD_ITERS_LEGACY` = 150 000). The count is part of the stored string, so existing hashes verify
  unchanged; `password_needs_rehash()` reports them.
- HTTP: **deferred responses.** `http_defer()` in a handler, then `http_deferred_response()` /
  `http_deferred_finish()` on the loop thread later (e.g. from a pool completion). Pipelined requests on
  the connection wait (answers stay in order), the idle timeout pauses, a graceful drain waits for it; a
  finish while changes are pending is held behind the durability barrier like any response; a finish after
  the client left is dropped (`deferred_orphaned`). New `http_response.deferred`, `http_conn.pending`.
- HTTP: `http_server_watch()` / `http_server_unwatch()`: extra descriptors on the server's poller (kqueue,
  epoll and poll), with a callback on the loop thread.
- HTTP: **IPv6.** `http_server_listen()` takes `"127.0.0.1"` / `"0.0.0.0"` (IPv4 only), `"::1"` / `"[::1]"` /
  any IPv6 literal (IPv6 only), `"::"`, `"*"`, `""` or NULL (dual stack: IPv4 clients arrive IPv4-mapped;
  falls back to 0.0.0.0 on a host without IPv6) or a name (its first address). New
  `http_server.listen_family` / `listen_dual_stack`. The listening socket is close-on-exec.
- HTTP: `http_addr_format()`; `http_conn.ip` shows an IPv4-mapped peer (`::ffff:a.b.c.d`) as `a.b.c.d`, so a
  client has the same rate-limiter key on an IPv4 and a dual-stack socket.
- Sessions: `mx_ip_normalize()` (canonical IPv4 / IPv6 text; accepts `[v6]:port`, `v4:port`, zones,
  IPv4-mapped) and `mx_ip_rate_key()` (IPv4 as is, IPv6 per /64 by default: `2001:db8:1:2::/64`).
  `xff_rightmost()` now returns the normalized address and rejects what is not an address literal (it used
  to accept any run of hex digits, dots and colons, such as `1.2.3`).
- **Install / packaging.** `make install` / `make uninstall` with `PREFIX`, `DESTDIR`, `LIBDIR`,
  `INCLUDEDIR`, `SHARED` (default 1): headers to `include/modx`, `libmodx.a`, the shared library
  (`libmodx.so.<version>` with soname `libmodx.so.<abi>`, or `libmodx.<version>.dylib` with install name
  `libmodx.<abi>.dylib`; the ABI version is `0.<minor>` while the major is 0), `modx-core.pc`
  (`pkg-config --libs modx-core`; `--static` adds `-lz -lm`) and a CMake package. The build tree's
  archive is still `build/libmodxcore.a`.
- **CMake**: `CMakeLists.txt` builds the library as `modx::core` (alias of `modx-core`, output name `modx`),
  installs it with an exported config package (`find_package(modx-core 0.2)`, same-minor compatibility)
  and the pkg-config file, works with `add_subdirectory` / `FetchContent` (install rules and tests only at
  top level), and runs the unit tests through `ctest`. Options `MODX_ZLIB`, `BUILD_SHARED_LIBS`,
  `MODX_INSTALL`, `MODX_BUILD_TESTS`.
- `make test-install` (`tests/install_test.sh`): installs into a temporary prefix and builds
  `examples/install/hello.c` with pkg-config (shared and static), CMake `find_package` against both the
  make- and the cmake-installed prefix, and `add_subdirectory`; checks DESTDIR staging and that uninstall
  leaves nothing. CI runs it on Linux and macOS, plus a CMake build with `ctest` and a ThreadSanitizer job.
- **Documentation.** Doxygen comments on every public function, type, field and macro in
  `include/modx/*.h`; a `Doxyfile` and `make docs` (output `build/docs/html`, skipped without doxygen);
  hand-written `docs/`: index, getting started, guides (HTTP and routing, store and durability including the
  read-only state after a failure and the recovery / salvage rules, sessions and passwords including async
  hashing, SSE, WebSocket, idempotency, OT) and a per-header reference summary.
- `examples/minimal/minimal.c` (`build/minimal`): the server the getting-started guide walks through, with a
  deferred login on the worker pool.
- `secure_zero()`; PBKDF2 wipes its HMAC state and no longer allocates for salts up to 256 bytes.
- Fork safety: background compaction forks from the loop thread; its child only serializes the snapshot
  and writes it (malloc, write, fsync, rename) and never touches the pool; workers hold no libc or modx
  lock while they compute, so they cannot leave one locked in the child. A test forks compactions while
  four workers hash.
- `make test-tsan`: the unit tests under ThreadSanitizer (pool, async hashing, the new in-process socket
  tests in `tests/test_net.c`).
- Store: **snapshot checksum.** Snapshots start with `{"crc32":"<hex8>/<len12>",`: the CRC-32 and the byte
  length of everything after that prefix. A mismatch fails the open (salvage loads it if it still parses).
  A snapshot whose length differs from the header but that parses was rewritten by another tool (an
  operator's `jq` / `JSON.stringify` edit): it is loaded without the check, with a warning
  (`snapshot_unchecked`); a truncated one does not parse and fails as before. The file is still plain JSON,
  so 0.2 readers open it; snapshots without the key (0.1 / 0.2) are read without the check. The journal
  format is unchanged (records always had a CRC).

## 0.2.0 — 2026-11-25

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
  hash index; each new key sweeps a few stored ones for aged-out keys; when full it drops every aged-out
  key, then evicts the oldest unblocked one. `rl_reset` removes
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
