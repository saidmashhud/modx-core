# API reference

This page summarizes every public header in `include/modx/`. The full generated reference comes from
`make docs` (Doxygen; output in `build/docs/html/index.html`; the target is skipped when `doxygen` is
missing). This page is the summary: purpose, names, one-line descriptions and the rules that matter.

Headers are included as `"modx/<name>.h"`, e.g. `#include "modx/http.h"`. Link the library with
`pkg-config --libs modx-core` or the CMake target `modx::core` (see [getting-started.md](getting-started.md)).
Linking needs `-pthread`; both the pkg-config file and the CMake target pass it.

General rules:

- The library is single-threaded around one event loop. Unless a section says otherwise, functions are
  not thread-safe and run on the loop thread. The only threads are the workers of `modx/pool.h`.
- Allocating functions abort on out-of-memory (see [util.h](#utilh)); they do not return NULL for it.
- "Borrowed" means the pointer belongs to someone else and is valid only for the stated lifetime.
  "Caller frees" means the result is `malloc`'ed and owned by the caller.

## Contents

- [cache.h](#cacheh): seq-validated response cache
- [crypto.h](#cryptoh): hashes, HMAC, PBKDF2, passwords, randomness, encodings
- [gzip.h](#gziph): gzip compression
- [http.h](#httph): HTTP/1.1 server and event loop
- [idem.h](#idemh): idempotency keys
- [json.h](#jsonh): JSON values
- [ot.h](#oth): operational transformation on text
- [pool.h](#poolh): worker thread pool
- [session.h](#sessionh): cookies, CSRF, client addresses, throttling, tokens
- [sse.h](#sseh): Server-Sent Events
- [static.h](#statich): static files
- [store.h](#storeh): durable document store
- [timer.h](#timerh): periodic jobs
- [util.h](#utilh): buffers, allocators, clocks, ids, strings
- [version.h](#versionh): version macros
- [ws.h](#wsh): WebSocket

## cache.h

A small response cache keyed by string and validated by store seq: an entry is valid only for the seq it
was built at, so any committed change makes it stale without explicit invalidation. Bounded by bytes,
LRU eviction. Not thread-safe; lookups are linear in the number of entries.

| Name | Description |
| --- | --- |
| `mx_cache_entry` | One cached body: owned `key`, `body`, `len`, `seq`, LRU clock `used`. |
| `mx_cache` | The cache: entries, `bytes` / `max_bytes`, `hits` / `misses` counters. |
| `mx_cache_init` | Empty cache bounded to `max_bytes` (0 caches nothing). |
| `mx_cache_free` | Free every entry and the array. |
| `mx_cache_get` | Body cached under key at exactly `seq`, or NULL; a stale entry is dropped. |
| `mx_cache_put` | Store a copy under key for seq, evicting LRU entries to fit. |
| `mx_cache_clear` | Drop every entry (counters are kept). |

Rules: the body returned by `mx_cache_get` is borrowed until the next put, clear or miss. Entries whose
key plus body exceed `max_bytes / 4` are not cached.

## crypto.h

SHA-256, HMAC-SHA256, PBKDF2, password hashing (synchronous and on a worker pool), OS randomness,
constant-time compare, hex and base64. SHA-1 exists only for the WebSocket handshake.

Digests and MACs:

| Name | Description |
| --- | --- |
| `SHA256_LEN`, `SHA256_BLOCK` | 32-byte digest, 64-byte block. |
| `sha256_ctx`, `sha256_init`, `sha256_update`, `sha256_final` | Incremental SHA-256; re-init before reuse. |
| `sha256` | One-shot SHA-256. |
| `hmac_sha256` | HMAC-SHA256 (RFC 2104). |
| `pbkdf2_sha256` | PBKDF2-HMAC-SHA256 (RFC 8018). |
| `SHA1_LEN`, `sha1` | SHA-1 for protocol needs only (not for secrets). |

Randomness, comparison, encodings:

| Name | Description |
| --- | --- |
| `crypto_random` | Fill a buffer from the OS CSPRNG; 0, or -1 when no source exists. |
| `ct_equal` | Constant-time compare; 1 = equal. |
| `secure_zero` | Zero memory with volatile stores the compiler keeps (wiping secrets). |
| `hex_encode`, `hex_decode` | Lowercase hex out (`2*len+1` bytes); decode returns bytes or -1. |
| `base64_encode`, `base64_decode` | Padded standard base64; out needs `4*((len+2)/3)+1` bytes; decode returns bytes or -1. |

Passwords:

| Name | Description |
| --- | --- |
| `PASSWORD_ITERS` | 600000: PBKDF2 iterations for new hashes (OWASP 2023). |
| `PASSWORD_ITERS_LEGACY` | 150000: the count used by earlier releases; such hashes still verify. |
| `PASSWORD_SALT_LEN` | 16 salt bytes. |
| `password_hash` | New `"pbkdf2_sha256$<iters>$<salt hex>$<hash hex>"` string at `PASSWORD_ITERS`; caller frees; NULL without randomness. |
| `password_hash_iters` | Same with an explicit iteration count. |
| `password_verify` | 1 if the password matches the stored string (constant-time digest compare), else 0. |
| `password_needs_rehash` | 1 when the stored hash has fewer than `min_iters` (0 = `PASSWORD_ITERS`) or is not a hash this library writes. |
| `mx_pool` | Worker pool type (forward declaration of [pool.h](#poolh)). |
| `password_hash_cb` | Completion `(ctx, char *hash)`; the callback owns and frees `hash`. |
| `password_verify_cb` | Completion `(ctx, int ok, int needs_rehash)`. |
| `password_hash_async` | Hash on a pool worker; `iters` 0 = `PASSWORD_ITERS`. |
| `password_verify_async` | Verify on a pool worker; same rules as `password_hash_async`. |

Rules:

- `password_hash` and `password_verify` are synchronous and cost about half a second of CPU at
  `PASSWORD_ITERS`. In a server use the async variants with `http_defer` (see
  [guides/sessions.md](guides/sessions.md)).
- The async functions copy the password (wiped after use), so the caller's buffer may go away after the
  call. The callback runs on the thread that runs the pool's completions (the loop, with `mx_pool_attach`).
- Return 0: the callback runs exactly once. Return -1: nothing was queued and the callback never runs
  (`errno` `EIO` without randomness, `EAGAIN` when the pool queue is full); answer 503 or fall back to the
  sync call.
- An invalid stored string makes `password_verify_async` report `ok = 0` without the PBKDF2 work, still
  through the pool.

## gzip.h

gzip response compression through the system zlib. Building with `ZLIB=0` leaves it out; everything then
reports "unavailable" and responses go out uncompressed.

| Name | Description |
| --- | --- |
| `mx_gzip_available` | 1 when compression is compiled in. |
| `mx_gzip` | Append the gzip encoding of a buffer to an `sbuf`; level 1..9, other = 6; 0 or -1 (out unchanged). |
| `mx_gunzip` | Inflate a gzip stream into an `sbuf`; 0 or -1 (out may hold partial output). |
| `mx_accepts_gzip` | 1 when an `Accept-Encoding` value allows gzip (directly or via `*`, honours `q=0`). |
| `mx_compressible` | 1 for content types worth compressing (text, JSON, JS, CSS, SVG, XML, manifest). |

The server compresses automatically through `http_server.gzip_min` / `gzip_level`.

## http.h

HTTP/1.1 server: request parser, single-threaded event loop (kqueue, epoll or poll), keep-alive and
pipelining, file streaming, gzip, SSE and WebSocket upgrades, deferred responses, watched descriptors and
a durability barrier (group commit) for responses that report writes.

Everything runs on the loop thread: handlers, ticks, watch callbacks and `http_deferred_finish`. None of
these functions is thread-safe.

Limits and timeouts (macros):

| Name | Default | Meaning |
| --- | --- | --- |
| `HTTP_MAX_BODY` | 1 MiB | Per-request body limit; larger needs `http_server.body_limit`. |
| `HTTP_MAX_UPLOAD` | 50 MiB | Hard ceiling for routes that opt in; larger gets 413. |
| `HTTP_READ_BUDGET` | 1 MiB | Bytes read from one connection per poll round. |
| `HTTP_FILE_CHUNK` | 256 KiB | Chunk size for streamed files. |
| `HTTP_MAX_HEADER` | 64 KiB | Request line + headers; larger gets 431. |
| `HTTP_MAX_CONNS` | 1024 | Default `max_conns`. |
| `HTTP_IDLE_MS` | 60 s | Idle keep-alive connection timeout. |
| `HTTP_HEADER_TIMEOUT_MS` | 10 s | First byte to end of headers. |
| `HTTP_BODY_TIMEOUT_MS` | 60 s | First byte to end of body. |
| `HTTP_WRITE_TIMEOUT_MS` | 60 s | Pending output without progress. |
| `HTTP_DRAIN_MS` | 5 s | Graceful shutdown budget. |
| `HTTP_LINGER_MS` | 2 s | Read-and-discard time after a final response. |
| `HTTP_SSE_MAX_BACKLOG` | 8 MiB | Unsent bytes above which `http_conn_send` marks a connection dead. |
| `HTTP_DURABLE_UNAVAILABLE` | -2 | `durable_commit` result meaning "storage refuses writes": held responses become 503. |

Types:

| Name | Description |
| --- | --- |
| `http_kv` | Key / value pair (query param or header), owned by the request; header names lower-cased. |
| `http_request` | Parsed request: `method`, `target`, decoded `path`, `query`, `headers`, `body`, `body_len`, `body_borrowed`, `keep_alive`. |
| `http_response` | What a handler fills: `status`, `content_type`, `body`, `headers` (`"Name: value\r\n"` lines), `file_fd` / `file_len`, and the `sse` / `ws` / `deferred` flags set by the library. |
| `http_conn` | One client connection, owned by the server. Applications read `ip`, `app` and `srv`. |
| `http_server` | Server state and options (see below). |
| `http_deferred` | Opaque handle for a response finished later. |
| `http_handler` | `(srv, conn, req, resp, ctx)`: request handler. |
| `http_tick` | `(srv, ctx)`: called once per loop iteration, after waiting, before the barrier. |
| `http_body_limit_fn` | `(partial_req, ctx)`: body limit for a request whose headers are parsed; consulted only above `HTTP_MAX_BODY`, clamped to `HTTP_MAX_UPLOAD`. |
| `http_fd_cb` | `(srv, fd, ctx)`: callback for a watched descriptor. |
| `http_watch` | Internal bookkeeping entry of `http_server_watch`. |

`http_server` setup: zero it, set `listen_fd = -1`, set `handler` (required) and options, then
`http_server_listen` and `http_server_run`. Option fields left 0 / NULL take the default.

| Group | Fields |
| --- | --- |
| Listening (set by listen) | `listen_fd`, `port`, `listen_family` (`AF_INET` / `AF_INET6`), `listen_dual_stack` (1 = IPv6 socket that accepts IPv4 too) |
| Callbacks | `handler`, `tick`, `body_limit`, `on_close`, `app_free`, `on_ws_message`, `ctx` (passed to handler, tick, on_close, on_ws_message, body_limit) |
| Limits / timeouts | `max_conns`, `idle_ms`, `header_timeout_ms`, `body_timeout_ms`, `write_timeout_ms`, `drain_ms` |
| Durability barrier | `durable_pending`, `durable_commit`, `durable_ctx` |
| gzip | `gzip_min` (0 = off), `gzip_level` (0 = 6) |
| WebSocket | `ws_max_message` (0 = `MX_WS_MAX_MESSAGE`), `ws_ping_ms` (0 = `MX_WS_PING_MS`) |
| Poller | `use_poll` (1 = force poll()) |
| Counters | `timeouts`, `nconns_peak`, `gzip_in` / `gzip_out`, `ws_messages`, `commits`, `commit_failures`, `deferred_started` / `deferred_finished` / `deferred_orphaned` |
| Watches | `watches`, `nwatches` |

Request helpers and parser:

| Name | Description |
| --- | --- |
| `http_req_header` | First value of a header (pass the lower-case name), or NULL. |
| `http_req_header_last` | Last occurrence of a header (e.g. the `X-Forwarded-For` a proxy appended). |
| `http_req_query` | First value of a query parameter, or NULL. |
| `http_status_text` | Reason phrase ("Unknown" if unlisted). |
| `http_url_decode` | Percent-decode; caller frees; NULL if a NUL byte would result. |
| `http_parse_request` | Parse one request: >0 bytes consumed, 0 need more, <0 = -status (400, 411, 413, 431). |
| `http_parse_request_ex` | Same, with a body-limit callback. |
| `http_parse_request_need` | Same; also reports the total bytes the request needs once headers are complete. |
| `http_request_free` | Free what the parser allocated (not a borrowed body) and zero the request. |

Server lifecycle and I/O:

| Name | Description |
| --- | --- |
| `http_server_listen` | Listen on host:port (port 0 = any free port); 0 or -1 with errno (`EINVAL` for a bad port or host). |
| `http_addr_format` | Format an `AF_INET` / `AF_INET6` sockaddr without port; IPv4-mapped shown as IPv4; 1 or 0. |
| `http_server_poller` | `"kqueue"`, `"epoll"` or `"poll"`. |
| `http_server_run` | Run until `*stop` is non-zero, then drain (bounded by `drain_ms`). |
| `http_server_close` | Final barrier, flush, free connections, close listening and poller fds (watched fds stay open). |
| `http_conn_send` | Queue bytes on a connection (SSE / WebSocket pushes); respects the barrier. |
| `http_server_watch` | Call a callback on the loop whenever an fd is readable (level-triggered). |
| `http_server_unwatch` | Stop watching an fd (it stays open); -1 / `ENOENT` if unknown. |

Deferred responses:

| Name | Description |
| --- | --- |
| `http_defer` | Inside a handler: ignore `resp` and wait for a later finish; returns the handle. |
| `http_deferred_response` | The response to fill (status 200, JSON content type, empty at start). |
| `http_deferred_conn` | The connection, or NULL once closed. |
| `http_deferred_alive` | 1 while the connection is open and not dead. |
| `http_deferred_finish` | Send (or hold for the barrier) the response, resume pipelined requests, free the handle. |

Host forms for `http_server_listen`:

| Host | Result |
| --- | --- |
| `"127.0.0.1"`, `"0.0.0.0"` | IPv4 only. |
| `"::1"`, `"[::1]"`, `"2001:db8::5"` | IPv6 only (`IPV6_V6ONLY`). |
| `"::"`, `"*"`, `""` or NULL | Dual stack; IPv4 clients appear as `a.b.c.d` in `http_conn.ip`; falls back to `0.0.0.0` without IPv6. |
| `"localhost"` or another name | The first address the name resolves to. |

Rules:

- `req` and `resp` are valid only during the handler call. Request strings are owned by the request.
- `resp->content_type` is not copied and must outlive the response.
- A request body above `HTTP_MAX_BODY` is a borrowed, not NUL-terminated view into the connection's input
  (`body_borrowed = 1`), valid until the handler returns.
- `file_fd >= 0` streams `file_len` bytes from the fd; the server takes it over and closes it; `body` must
  be empty.
- Barrier: a response produced while `durable_pending` is true is held; once per iteration
  `durable_commit` runs (one write, one fsync) and then held responses go out. A failed commit turns them
  into 500 (or 503 when it returns `HTTP_DURABLE_UNAVAILABLE` or less) and closes the connection. Pushes
  through `http_conn_send` made while changes are pending go out after the commit (dropped if it fails).
- `http_server_watch`: one callback per fd; works before or while the server runs; `cb` must consume what
  made the fd readable. Errors: `EINVAL` (fd < 0 or NULL cb), `EEXIST`, or the poller's error.
- Deferred: exactly one `http_deferred_finish` per `http_defer`, on the loop thread. Pipelined requests on
  the connection wait; idle timeouts pause; a drain waits (bounded by `drain_ms`). If the client left, the
  response object stays writable and finish only frees it (`deferred_orphaned`). Deferred responses cannot
  be SSE or WebSocket.
- `on_close` runs for every connection with `app` set right before it is freed (never on shutdown);
  `app_free` frees `http_conn.app` (NULL = `free`).

See [guides/http.md](guides/http.md).

## idem.h

Idempotency keys: a client retrying a write sends the same `Idempotency-Key`, and the server replays the
first completed answer instead of executing the write again. The module is the mechanism; the application
picks the collection (private, no-ring), the record id (key scope) and retention.

| Name | Description |
| --- | --- |
| `MX_IDEM_HEADER` | `"idempotency-key"`, lower-cased for `http_req_header`. |
| `MX_IDEM_KEY_MAX` | 128: longest accepted key in bytes. |
| `MX_IDEM_REPLAYED_HEADER` | `"Idempotent-Replayed: true\r\n"`, added on replay. |
| `mx_idem_key_valid` | 1 for 1..128 printable ASCII chars, no leading / trailing space. |
| `mx_idem_method` | 1 for POST, PUT, PATCH, DELETE. |
| `mx_idem_storable` | 1 for buffered 2xx and 4xx responses except 408 and 429 (no 5xx, SSE, WebSocket, file). |
| `mx_idem_record` | New record row for the answer (caller owns); oversized or non-UTF-8 bodies get `body_omitted:true`. |
| `mx_idem_matches` | 1 when the record was made for the same method and target. |
| `mx_idem_expired` | 1 when the record is at least `ttl_ms` old (no `created_at` = expired). |
| `mx_idem_replay` | Fill a response from a record (status, type, body, replay header). |
| `mx_idem_prune` | Delete records older than a cutoff (or without numeric `created_at`); count or -1. |
| `mx_idem_cap` | Delete an owner's oldest records until fewer than `max` remain; count or -1. |

Rules: put the record in the same store batch as the write, before the barrier commits, so both are
durable or neither. `mx_idem_replay` may borrow `content_type` from the record, which must stay in the
store until the response is serialized. Call `mx_store_sync` after prune / cap. -1 means the store has
failed (`mx_store_failed`). A replay of an omitted body answers the stored status with
`{"replayed":true,"body_omitted":true}`.

See [guides/idempotency.md](guides/idempotency.md).

## json.h

JSON values: parser, builders, accessors and serializer. Every value is a heap tree owned by whoever holds
the root; containers own their children.

| Name | Description |
| --- | --- |
| `json_type` | `JSON_NULL`, `JSON_BOOL`, `JSON_NUMBER`, `JSON_STRING`, `JSON_ARRAY`, `JSON_OBJECT`. |
| `json` | A value: `type`, `boolean`, `number` (double), `str` / `str_len`, `items`, `keys`, `len` / `cap`. |
| `json_parse` | Parse exactly one value; caller frees; NULL with a message in `err`. Max nesting 256; repeated key keeps last. |
| `json_free` | Free a value tree (NULL is a no-op). |
| `json_null`, `json_bool`, `json_num`, `json_str`, `json_strn`, `json_arr`, `json_obj` | Constructors (strings are copied; `json_strn` may contain NULs). |
| `json_clone` | Deep copy. |
| `json_equal` | Structural equality (key order ignored). |
| `json_get` | Value of a key (borrowed), or NULL. |
| `json_set` | Set a key; takes ownership; replaces and frees an existing value. |
| `json_del` | Remove a key and free its value. |
| `json_has` | 1 when an object has the key. |
| `json_push` | Append to an array; takes ownership. |
| `json_get_str` | String value of a key (borrowed), or NULL. |
| `json_get_num` | Number value of a key, or a fallback. |
| `json_is_null_or_missing` | 1 when the key is missing or null. |
| `json_is_int` | 1 for a finite integral number in the int53 range. |
| `json_write` | Append compact serialization to an `sbuf` (non-finite numbers as null). |
| `json_write_string` | Append a quoted, escaped JSON string. |
| `json_dump` | Serialize to a new string; caller frees. |

Rules: `json_set` and `json_push` take ownership of the value passed in; getters return borrowed pointers
valid while the parent lives and the key is not replaced. Object keys keep insertion order.

## ot.h

Operational transformation on plain text (the ot.js TextOperation model). Lengths and offsets are UTF-16
code units, as JavaScript counts them; the text is stored as UTF-8. Wire form: an array of numbers
(retain n), non-empty strings (insert) and `{"d": n}` (delete n).

| Name | Description |
| --- | --- |
| `OT_MAX_TEXT` | 1000000: max document length in UTF-16 units. |
| `OT_MAX_ITEMS` | 10000: max components in one request. |
| `OT_RETAIN`, `OT_INSERT`, `OT_DELETE` | Component kinds. |
| `ot_comp` | One component: `kind`, `n`, insert text `s` / `slen` (owned). |
| `ot_op` | An operation: components, `base_len`, `target_len`. |
| `ot_init`, `ot_free` | Make empty / free components. |
| `ot_retain`, `ot_insert`, `ot_delete` | Append components with ot.js normalization (merge, insert before delete, zero ignored). |
| `ot_utf16_len` | UTF-16 length of UTF-8 text, or -1 when invalid. |
| `ot_from_json` | Parse the wire form (initializes `op`); 0 or -1 with `err`. |
| `ot_to_json` | Wire form as a new JSON array; caller frees. |
| `ot_is_noop` | 1 when the operation only retains. |
| `ot_transform` | `transform(a, b) -> (a2, b2)`; a's insert wins ties; 0 or -1 on base length mismatch. |
| `ot_apply` | Apply to UTF-8 text, appending to an `sbuf` (NULL = validate only); 0 or -1 with `err`. |

Rules: an operation spans its base exactly (retains + deletes = base length). A retain or delete may not
end inside a surrogate pair. Output ops of `ot_from_json` and `ot_transform` are initialized by the call;
on error they are empty.

See [guides/ot.md](guides/ot.md).

## pool.h

Worker thread pool for CPU-bound work that must not block the loop (password hashing). A job's `work`
runs on a worker; its `done` runs later on the thread that calls `mx_pool_run_done`, normally the loop,
woken through a pipe registered with `mx_pool_attach`.

| Name | Description |
| --- | --- |
| `mx_pool` | Opaque pool. |
| `mx_job_fn` | `(arg)`: runs on a worker thread. |
| `mx_done_fn` | `(arg)`: runs on the completion (loop) thread. |
| `MX_POOL_THREADS` | 2: default worker count. |
| `MX_POOL_MAX_QUEUE` | 1024: default bound on waiting jobs. |
| `mx_pool_new` | Start workers (<= 0 = default) with a queue bound (0 = default); NULL on failure. |
| `mx_pool_submit` | Queue a job from the loop thread; 0 or -1 with errno. |
| `mx_pool_run_done` | Run finished jobs' `done` callbacks in completion order; non-blocking; count. |
| `mx_pool_wait` | Wait up to `timeout_ms` (-1 forever) for a completion, then run them (tools, tests). |
| `mx_pool_fd` | Descriptor readable while completions wait (custom event loops). |
| `mx_pool_attach` | Register `mx_pool_fd` with `http_server_watch` so the server runs completions; 0 or -1. |
| `mx_pool_busy` | Jobs queued, running or finished but not yet done (takes the mutex). |
| `mx_pool_stats`, `mx_pool_stats_get` | Snapshot: `threads`, `submitted`, `completed`, `rejected`, `queued`, `running`. |
| `mx_pool_free` | Stop accepting, finish queued jobs, join, run remaining `done`, free (NULL is a no-op). |

Rules:

- `work` touches only the job's own memory: no store, HTTP or other loop-owned state, no stdio, preferably
  no malloc (the store's background compaction forks from the loop thread).
- `arg` stays the caller's; free it in `done`. `done` may be NULL.
- `mx_pool_submit` errors: `EAGAIN` (queue full), `EPIPE` (stopping), `ECHILD` (called in a forked child);
  then neither function runs.
- If only some threads start, the pool runs with those (`mx_pool_stats.threads`).
- Call `mx_pool_free` after `http_server_close`, or detach first with `http_server_unwatch` if the server
  lives on.

## session.h

Reusable pieces of cookie sessions: cookie parsing, CSRF origin checks, proxy-aware client addresses,
failure throttling and session tokens. Where sessions are stored is up to the application (usually a
private store collection keyed by the token hash).

Requests and addresses:

| Name | Description |
| --- | --- |
| `cookie_get` | Copy cookie `name` from a Cookie header (quotes removed); 1 if found and it fit. |
| `origin_matches_host` | CSRF: 1 if the Origin's host:port equals Host (http / https only; `null` and missing fail). |
| `content_type_is_json` | 1 for `application/json` with optional parameters, case-insensitive. |
| `xff_rightmost` | Rightmost `X-Forwarded-For` element, normalized; 1, or 0 (fall back to `http_conn.ip`). |
| `mx_ip_normalize` | Canonical IP text (`inet_ntop` form); accepts blanks, `[v6]`, `[v6]:port`, `v4:port`, zones (dropped), maps `::ffff:a.b.c.d` to IPv4; 1 or 0. |
| `MX_IP_V6_PREFIX` | 64: default IPv6 prefix for rate-limit keys. |
| `mx_ip_rate_key` | Rate-limit key: IPv4 itself; IPv6 cut to `v6_prefix` bits (0 = 64, max 128) as `"2001:db8:1:2::/64"`; 1 or 0. |

Failure throttling:

| Name | Description |
| --- | --- |
| `MX_RL_MAX_FAILS` | 10 failures inside the window block a key. |
| `MX_RL_WINDOW_MS` | 10-minute sliding window. |
| `MX_RL_CAPACITY` | 4096: default number of keys kept. |
| `rl_entry`, `ratelimit` | Per-key failure ring; the bounded, hash-indexed throttle. |
| `rl_init`, `rl_init_cap` | Empty throttle with the default or a given capacity (allocates nothing). |
| `rl_free` | Free the tables (same capacity remains). |
| `rl_blocked` | 1 when the key has `MX_RL_MAX_FAILS` failures in the window ending at `now`. |
| `rl_fail` | Record a failure. |
| `rl_reset` | Forget a key (after a successful login). |

Tokens:

| Name | Description |
| --- | --- |
| `session_token_new` | New 64-hex-char token and its hex SHA-256 (store the hash); 0 or -1 without randomness. |
| `session_token_hash` | Hex SHA-256 of a presented token, for lookup. |

Rules: `ratelimit` is not thread-safe. Keys are used as given, so pass `mx_ip_rate_key` output for
addresses (an IPv6 client rotating through its /64 then stays one key). When full, keys whose failures
aged out are dropped first, then the key with the oldest last failure, preferring unblocked keys
(`evictions` counts drops inside the window). Use `xff_rightmost` only behind a trusted proxy.

See [guides/sessions.md](guides/sessions.md).

## sse.h

Server-Sent Events on the HTTP server: a handler turns its response into a stream; a hub fans events out
to every stream a filter accepts and sends comment pings to keep idle proxies awake. Loop thread only;
pushes go through `http_conn_send`, so they respect the barrier.

| Name | Description |
| --- | --- |
| `mx_sse` | Hub: `srv`, `ping_ms`, `last_ping`. |
| `mx_sse_init` | Initialize a hub (`ping_ms <= 0` = 15000). |
| `mx_sse_open` | Turn a response into an event stream and bind `app` to the connection; writes `retry: 2000`. |
| `mx_sse_format` | Append one event (`id:` if >= 0, `event:` if non-NULL, `data:` per line). |
| `mx_sse_filter` | `(ctx, conn)`: non-zero = send to this stream. |
| `mx_sse_broadcast` | Send to every live stream the filter accepts (NULL = all); returns streams told. |
| `mx_sse_send` | Send to one stream. |
| `mx_sse_tick` | Comment ping every `ping_ms`; call from the server tick. |
| `mx_sse_count` | Number of live streams. |

Rules: `app` is freed by `http_server.app_free` when the connection closes. Stream responses are never
gzipped or held by the barrier.

See [guides/sse.md](guides/sse.md).

## static.h

Static file serving into an `http_response` (the whole file is read into the body), with MIME types,
weak ETags / 304 and an SPA fallback. Not thread-safe (keeps the last stat in a static); loop thread.

| Name | Description |
| --- | --- |
| `static_mime_type` | MIME type for a path's extension; `application/octet-stream` when unknown. |
| `static_serve` | Serve `req->path` under a root for GET / HEAD (method not checked). |
| `static_path_safe` | 1 if a decoded path is safe to map (leading `/`, no `\`, no `.` / `..` segments). |
| `static_serve_exact` | Serve exactly `root + rel` (no fallback, no ETag, no-cache); 0 or -1 (resp untouched). |

`static_serve` behaviour: `/` and directories map to `index.html`; unknown paths get the app shell
(`app.html`, else `index.html`, no-cache), except under `/assets/` (404). Files under `/assets/` are
immutable for a year, others no-cache. Unsafe paths get 400, a missing root or file 404.

## store.h

In-memory document store made durable by a snapshot file (`<dir>/snapshot.json`) and an append-only
journal (`<dir>/journal.log`). Rows are JSON objects with a string `"id"`; every put / del gets the next
seq, is journaled and applied in memory. `mx_store_sync` makes everything durable (one write, one fsync)
and only then hands changes to the listener. A ring of recent public changes serves catch-up.

Hash map (`hmap`, string key to owned JSON):

| Name | Description |
| --- | --- |
| `hentry`, `hmap` | Slot and open-addressing map. |
| `hmap_init`, `hmap_free` | Empty / free everything. |
| `hmap_get` | Value (borrowed), or NULL. |
| `hmap_put` | Store (key copied, value owned; replaced value freed). |
| `hmap_swap` | Store and return the replaced value (caller frees). |
| `hmap_del` | Remove and free; 1 or 0. |
| `hmap_take` | Remove and return the value (caller owns). |

Collections and hooks:

| Name | Description |
| --- | --- |
| `MX_COLL_PRIVATE` | Persisted, never a public change (sessions, secrets). |
| `MX_COLL_NO_RING` | Takes no slot in the change ring. |
| `MX_COLL_REDACT` | Public change JSON built with `hooks.write_public`. |
| `mx_coll_def` | `name`, `flags`, NULL-terminated `index` field list; its array index is the collection number. |
| `mx_index`, `mx_ixbucket` | Secondary index internals (use `mx_store_find`). |
| `mx_change` | Retained change: `seq`, `coll`, public `json` (NULL for private), `scope`. |
| `mx_commit_info` | Facts about a change passed to `scope_new` (borrowed during the call; `replay` = 1 on open). |
| `mx_store_hooks` | `ctx`, `row_key`, `journal_extra`, `write_public`, `scope_new`, `scope_free`, `on_apply`, `before_snapshot` (all optional). |
| `mx_store_listener` | `(ctx, change)`: called after a change became durable, in seq order. |
| `mx_store` | Store state; applications set `listener`, `listener_ctx`, `app`, `compact_threshold` (default 4 MiB), `sync_compaction` and read stats / recovery fields. |

Open, close, options and recovery:

| Name | Description |
| --- | --- |
| `mx_store_opts` | Open options: `salvage`, `report_path` (NULL = `<dir>/recovery-<unix ms>.txt`). Zero = defaults. |
| `mx_store_open` | Open or create the data dir, load the snapshot, replay `journal.old` and `journal.log`; NULL with `err` on fatal errors. |
| `mx_store_open_ex` | `mx_store_open` with options (`opts` may be NULL). |
| `mx_store_close` | Wait for background compaction, write and fsync queued records (unless failed), free (NULL no-op). |

Recovery fields on `mx_store` (set by open): `recovered_truncated`, `recovered_tail_bytes` (torn tail cut
off), `skipped_records` (records of unknown collections), `snapshot_unchecked` (snapshot rewritten outside
modx, loaded without its checksum), and in salvage mode `salvage`, `salvaged_records`, `salvaged_bytes`,
`seq_gaps`, `snapshot_crc_mismatch`, `recovery_report` (report path; NULL when nothing was damaged).

Reads:

| Name | Description |
| --- | --- |
| `mx_store_coll`, `mx_store_coll_name` | Collection number by name (-1) / name by number (NULL). |
| `mx_store_is_empty` | 1 when seq is 0 and every collection is empty. |
| `mx_store_get` | Row by storage key (borrowed until replaced or removed), or NULL. |
| `mx_store_count` | Rows in a collection. |
| `mx_store_all` | All rows: malloc'ed array of borrowed pointers (free the array only). |
| `mx_store_find` | Rows whose string field equals a value (index if declared, else scan); free the array, also when n is 0. |
| `mx_store_find_count` | Count `mx_store_find` would return (O(1) for indexed fields). |
| `mx_store_indexed` | 1 when (coll, field) is indexed. |

Writes, durability and failure:

| Name | Description |
| --- | --- |
| `mx_store_put` | Store a row (takes ownership, also on failure); 0 or -1. |
| `mx_store_del` | Remove by id (row_key not consulted); 0 or -1. |
| `mx_store_del_key` | Remove by storage key (NULL = id); `id` is what the change reports. |
| `mx_store_sync` | Write queued records, fsync once, notify the listener; 0, -1, or `MX_STORE_EFAILED` after a failure. |
| `MX_STORE_EFAILED` | -2: `mx_store_sync` result once the store has failed. |
| `mx_store_failed` | 1 once the store has failed (sticky). |
| `mx_store_dirty` | 1 when committed changes are not durable yet (after a failure: a mutation was refused since the last sync). |
| `mx_store_health`, `mx_store_health_get` | Health snapshot: `failed`, `fail_errno`, `fail_op`, `seq`, `synced_seq`, `pending_records`, `rejected_writes`, `journal_size`. |
| `mx_store_pending_cb`, `mx_store_commit_cb` | Adapters for `http_server.durable_pending` / `durable_commit` (ctx = store). |

Compaction, catch-up and housekeeping:

| Name | Description |
| --- | --- |
| `mx_store_compact` | Write the snapshot and truncate the journal in-process (blocks); 0 or -1. |
| `mx_store_compact_bg` | Rotate the journal, fork, child writes the snapshot; 0 started, 1 already running, -1. |
| `mx_change_filter`, `mx_change_fn` | Filter and visitor for catch-up. |
| `mx_store_for_each_since` | Visit durable retained public changes with seq > since; -1 when since is outside the window or ahead (client must re-bootstrap). |
| `mx_store_ring_at` | i-th retained change, oldest first (borrowed). |
| `mx_store_tick` | Reap a background compaction; call from the server tick. |

Rules:

- A store belongs to one thread (the loop). Background compaction forks; fork only from that thread and
  never touch the store from pool workers.
- Recovery on open: only a torn tail of `journal.log` is cut off. A bad record followed by valid ones, a
  seq gap, a seq going backwards, damage in `journal.old`, or a snapshot that fails its checksum or does
  not parse fails the open with a message in `err`.
- Salvage (`mx_store_opts.salvage` or `MODX_STORE_SALVAGE=1`) keeps every valid record, accepts gaps,
  skips backwards seqs, loads a checksum-failing snapshot if it parses, keeps damaged files as
  `<file>.damaged-<unix ms>`, writes a report and compacts the result into a fresh snapshot and empty
  journal.
- Mutations are not durable until `mx_store_sync`. A journal write or fsync error fails the store for
  good: queued records are dropped, mutations return -1 with `errno = EROFS`, compaction is refused, reads
  keep working, the catch-up feed stops at `synced_seq`. Recovery is a reopen.
- `mx_coll_def` arrays must outlive the store. Hooks are copied. Strings returned by `row_key` are freed by
  the store. Listener and visitor `mx_change` pointers are borrowed.

See [guides/store.md](guides/store.md).

## timer.h

Periodic jobs for the event loop: register callbacks with an interval and call `mx_timers_run` from the
server tick. Times are monotonic ms (`mono_ms`).

| Name | Description |
| --- | --- |
| `mx_timer_fn` | `(ctx, now)`: timer callback. |
| `mx_timer`, `mx_timers` | One job (`fn`, `ctx`, `every_ms`, `next_at`); growable set. |
| `mx_timers_init`, `mx_timers_free` | Empty set / free (reusable afterwards). |
| `mx_timers_add` | Add a timer (`every_ms < 1` = 1; `first_at` 0 = now + every_ms); returns its index. |
| `mx_timers_run` | Run every due timer once (missed runs skipped); count. |
| `mx_timers_next_in` | Ms until the next timer is due, or -1 without timers. |

Timers cannot be removed.

## util.h

Byte buffers, aborting allocators, CRC-32, clocks, ids and small string helpers. The `x*` allocators and
every function that allocates through them print "out of memory" and `abort()` instead of returning NULL.

| Name | Description |
| --- | --- |
| `sbuf` | Growable byte buffer (`data`, `len`, `cap`); NUL-terminated when `data != NULL`. |
| `sbuf_init`, `sbuf_free` | Empty / free. |
| `sbuf_reserve`, `sbuf_reserve_exact`, `sbuf_shrink` | Grow (doubling from 64) / grow exactly / give back spare capacity. |
| `sbuf_put`, `sbuf_puts`, `sbuf_putc`, `sbuf_printf` | Append bytes, string, byte, formatted text. |
| `sbuf_consume` | Drop the first n bytes. |
| `sbuf_take` | Detach the buffer (caller frees; never NULL, empty gives `""`). |
| `xmalloc`, `xcalloc`, `xrealloc`, `xstrdup`, `xstrndup` | Aborting allocators (`xmalloc(0)` allocates 1). |
| `crc32_buf` | CRC-32 (IEEE, as zlib). |
| `now_ms` | Wall-clock Unix ms. |
| `mono_ms` | Monotonic ms, for durations. |
| `rand_u64` | Non-cryptographic, not thread-safe random 64-bit value (use `crypto_random` for secrets). |
| `gen_id` | `"<prefix>_<base36 ms><4 random base36>"`; caller frees. |
| `str_icase_eq`, `str_icase_contains`, `str_icase_find` | ASCII case-insensitive equality / substring / offset (-1). |
| `utf8_count` | UTF-8 code points in n bytes. |
| `text_snippet` | Up to `max_chars` code points around a match, with `…` on cut sides; caller frees. |

## version.h

Version macros (semantic versioning, matching CHANGELOG.md).

| Name | Description |
| --- | --- |
| `MODX_VERSION_MAJOR`, `MODX_VERSION_MINOR`, `MODX_VERSION_PATCH` | Components. |
| `MODX_VERSION` | Version string of the headers, e.g. `"0.3.0"`. |
| `MODX_VERSION_NUM` | `MAJOR * 10000 + MINOR * 100 + PATCH`, for `#if`. |
| `modx_version` | Version the library was built as; compare with `MODX_VERSION` to catch mismatched headers and objects. |

## ws.h

WebSocket (RFC 6455) on the same server and loop as SSE. A handler accepts the upgrade; complete messages
go to `http_server.on_ws_message`; pings are answered, closing is handled, idle sockets are pinged and
dropped if they stop answering. Close codes: 1002 protocol violation, 1007 invalid UTF-8, 1009 oversized.
No extensions. Loop thread only; sends go through `http_conn_send` and respect the barrier.

| Name | Description |
| --- | --- |
| `MX_WS_CONT`, `MX_WS_TEXT`, `MX_WS_BINARY`, `MX_WS_CLOSE`, `MX_WS_PING`, `MX_WS_PONG` | Opcodes. |
| `MX_WS_MAX_MESSAGE` | 1 MiB default per-message limit. |
| `MX_WS_PING_MS` | 30 s idle ping interval, also the answer deadline. |
| `mx_ws_accept` | Validate an upgrade and make resp the 101 answer; 0, or 400 / 426 for the caller to set. |
| `mx_ws_is_upgrade` | 1 for `Upgrade: websocket` with `Connection: upgrade`. |
| `mx_ws_accept_key` | `Sec-WebSocket-Accept` for a key (29 bytes incl. NUL). |
| `mx_ws_send` | Queue one unfragmented frame (no-op unless open and not closing). |
| `mx_ws_send_text` | Send a NUL-terminated text message. |
| `mx_ws_close` | Start the closing handshake (code 0 = none; reason up to 123 bytes). |
| `mx_ws_broadcast` | Send to every open socket the filter accepts (NULL = all); count. |
| `mx_ws_frame`, `mx_ws_parse_frame` | Frame header parser (tests, fuzzing): >0 frame bytes, 0 need more, -1002, -1009. |
| `mx_utf8_valid` | 1 for valid UTF-8 (no overlongs, surrogates, > U+10FFFF). |
| `mx_ws_on_input`, `mx_ws_tick` | Called by the server loop; not for applications. |

Rules: `app` passed to `mx_ws_accept` is bound to the connection like for SSE. On a refused handshake
resp is untouched except a `Sec-WebSocket-Version: 13` header on 426. `data` in `on_ws_message` is valid
only during the call. `subprotocol` is echoed in `Sec-WebSocket-Protocol`; there is no negotiation.

See [guides/websocket.md](guides/websocket.md).

## See also

- [README.md](README.md)
- [getting-started.md](getting-started.md)
- [guides/http.md](guides/http.md)
- [guides/store.md](guides/store.md)
- [guides/sessions.md](guides/sessions.md)
- [guides/sse.md](guides/sse.md)
- [guides/websocket.md](guides/websocket.md)
- [guides/idempotency.md](guides/idempotency.md)
- [guides/ot.md](guides/ot.md)
