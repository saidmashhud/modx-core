# modx-core

A small backend framework in C11: an HTTP/1.1 server on an event loop (kqueue / epoll / poll) with
Server-Sent Events and WebSocket, and a durable in-memory document store (snapshot + CRC-checked journal)
with a change feed for live clients. libc/POSIX plus the system zlib; nothing else at run time.

It is the generic half of a production issue tracker, extracted so that other servers can use it:
the application registers its collections, keys, redaction and visibility rules and its routes; the
framework does the I/O, durability and fan-out.

```
include/modx/   public headers           src/       implementation (libmodxcore.a)
tests/          unit tests + example e2e  fuzz/      fuzz targets (stand-alone driver and libFuzzer)
bench/          load generator + driver   examples/  todo (complete server), minimal, install (consumer)
docs/           guides + API reference    cmake/     package config templates
```

**Documentation:** [docs/](docs/README.md): [getting started](docs/getting-started.md), guides
([HTTP](docs/guides/http.md), [store and durability](docs/guides/store.md),
[sessions and passwords](docs/guides/sessions.md), [SSE](docs/guides/sse.md),
[WebSocket](docs/guides/websocket.md), [idempotency](docs/guides/idempotency.md), [OT](docs/guides/ot.md)) and
the [API reference](docs/reference.md); `make docs` generates the full reference from the headers (Doxygen).

## What is in it

| Module | Header | |
|---|---|---|
| HTTP server | `http.h` | HTTP/1.1 over IPv4 / IPv6 / dual stack, keep-alive, pipelining, `Expect: 100-continue`, per-route body limits, streamed file responses; kqueue (macOS/BSD), epoll (Linux), poll() fallback; connection limit; header / body / write / idle timeouts (slowloris); lingering close; graceful drain on shutdown; gzip; **durability barrier** (group commit) |
| Store | `store.h` | collections registered by the app; snapshot + journal (`<crc32> <json>\n`), torn-tail recovery, fail-stop on other damage, salvage mode; **group commit**; **background compaction** (fork + copy-on-write); change ring with catch-up; hooks for storage keys, redaction, change scopes and derived state; **secondary indexes** |
| SSE | `sse.h` | event framing, streams bound to app data, filtered broadcast, pings |
| WebSocket | `ws.h` | RFC 6455: handshake, masking, fragmentation, ping/pong, close codes 1002/1007/1009, limits, idle pings |
| JSON | `json.h` | parser and writer (UTF-8, `\u` escapes, surrogate pairs) |
| Crypto | `crypto.h` | SHA-256, HMAC, PBKDF2, SHA-1 (WebSocket only), base64, hex, constant-time compare, OS randomness, password hashes (PBKDF2-SHA256, 600 000 iterations, older counts still verify; **async** on the worker pool, rehash check) |
| Worker pool | `pool.h` | pthreads workers for CPU-bound jobs; completions run on the loop thread (the server's poller is woken through a pipe); bounded queue; fork-safe with background compaction |
| Sessions | `session.h` | cookie parsing, Origin/Host CSRF check, JSON content type, `X-Forwarded-For`, IPv4 / IPv6 address normalization and limiter keys (IPv6 per /64), failure throttling, session tokens (store the hash) |
| Static files | `static.h` | safe paths, MIME types, SPA fallback, immutable caching for hashed assets, weak ETag / 304 |
| Idempotency keys | `idem.h` | `Idempotency-Key` validation, remembered answers (2xx / 4xx) replayed with `Idempotent-Replayed: true`, method + target mismatch check, pruning by age and per-owner cap; records live in a private store collection |
| Response cache | `cache.h` | LRU of bodies valid only for the store `seq` they were built at |
| OT | `ot.h` | plain-text operational transformation (UTF-16 offsets, compatible with ot.js) |
| Timers | `timer.h` | periodic jobs for the loop tick |
| gzip | `gzip.h` | zlib deflate / inflate, `Accept-Encoding` parsing |
| Version | `version.h` | `MODX_VERSION` (`"0.3.0"`), major / minor / patch macros, `modx_version()` |

## Quick start

```sh
make            # build/libmodxcore.a and build/todo
make test       # unit tests + the example's end-to-end test (Node >= 18 for the e2e)
TODO_PORT=8080 ./build/todo   # then open http://127.0.0.1:8080
```

A server is a handler plus the store (abridged from `examples/todo/todo.c`):

```c
#include "modx/http.h"
#include "modx/sse.h"
#include "modx/store.h"

enum { C_TODOS, C_NCOLL };
static const char *const TODO_INDEX[] = { "list", NULL };
static const mx_coll_def COLLS[] = { { "todos", 0, TODO_INDEX } };

static void handle(http_server *srv, http_conn *c, const http_request *req, http_response *resp, void *ctx) {
    app *a = ctx;
    if (strcmp(req->path, "/api/events") == 0) { mx_sse_open(c, resp, NULL); return; }
    if (strcmp(req->method, "POST") == 0 && strcmp(req->path, "/api/todos") == 0) {
        json *row = /* validate req->body, build the row with an "id" */;
        mx_store_put(a->st, C_TODOS, row, http_req_header(req, "x-client-id"));
        resp->status = 201;  /* sent only after the change is on disk (see below) */
        ...
    }
}

static void on_durable(void *ctx, const mx_change *ch) {   /* after fsync, in seq order */
    mx_sse_broadcast(&((app *)ctx)->sse, NULL, NULL, NULL, ch->seq, ch->json);
}

int main(void) {
    char err[256];
    app a = { 0 };
    a.st = mx_store_open("./data", COLLS, C_NCOLL, 10000, NULL, err, sizeof err);
    http_server srv = { 0 };
    http_server_listen(&srv, "127.0.0.1", 8080);
    mx_sse_init(&a.sse, &srv, 15000);
    a.st->listener = on_durable;
    a.st->listener_ctx = &a;
    srv.durable_pending = mx_store_pending_cb;   /* group commit */
    srv.durable_commit = mx_store_commit_cb;
    srv.durable_ctx = a.st;
    srv.handler = handle;
    srv.ctx = &a;
    http_server_run(&srv, &stop);                /* until SIGTERM; then drains */
    http_server_close(&srv);
    mx_store_close(a.st);
}
```

## Install and use

```sh
make install PREFIX=/usr/local          # DESTDIR=… for staging; SHARED=0 for the archive only
make uninstall PREFIX=/usr/local
```

installs `include/modx/*.h`, `lib/libmodx.a`, the shared library (`libmodx.so.0.3.0`, soname
`libmodx.so.0.3`; `libmodx.0.3.0.dylib` on macOS), `lib/pkgconfig/modx-core.pc` and a CMake package. Then:

```sh
cc $(pkg-config --cflags modx-core) app.c $(pkg-config --libs modx-core)
```

```cmake
find_package(modx-core 0.3 REQUIRED)       # or add_subdirectory(modx-core) / FetchContent
target_link_libraries(app PRIVATE modx::core)
```

`CMakeLists.txt` builds and installs the same library (`cmake --install`), with an exported
`modx::core` target; the Makefile stays the primary build. `make test-install` checks every one of these
paths against a temporary prefix. Without installing: copy `include/` and `src/` into your tree (or add
this repository as a submodule), compile the sources with `-Iinclude -pthread` and link `-lz -lm
-pthread` (`ZLIB=0` / `-DMODX_NO_ZLIB` leaves zlib out).

## Guarantees

- **Acknowledged means durable.** Mutations queue their journal records. Once per loop iteration the
  durability barrier writes all of them with one `write()` and one `fsync()`, then hands the changes to
  the listener, then releases the responses. A response produced while changes are pending (the writer's,
  and any read that saw them) waits for that barrier; pushes made while changes are pending (SSE,
  WebSocket) are sent after the change notifications. If the commit fails the held responses become 500s
  and the queued pushes are dropped. The in-memory state is not rolled back on an I/O error (as before
  group commit): a 500 means "outcome unknown, re-read".
- **A failed commit is sticky.** After a journal `write()` or `fsync()` error the store is read-only until
  it is reopened: nothing is ever announced durable again, `fsync()` is never retried (a later one that
  "succeeds" proves nothing once one failed), mutations are refused (the barrier answers them with 503),
  and compaction is refused. Reads keep working. `mx_store_failed()` / `mx_store_health_get()` expose the
  state; a restart replays exactly what reached the disk.
- **Recovery.** A record is `<crc32 hex> <json>\n`; a snapshot starts with `{"crc32":"<hex>/<length>",`
  over the rest of the file (an edit by another tool changes the length and is loaded with a warning). A crash can only tear the end of the journal, so on open only the bytes after the last
  valid record are cut off (and logged). A bad record with valid records after it, a seq gap or a seq going
  backwards, damage in `journal.old`, or a snapshot that fails its checksum or does not parse make the open
  fail with a precise message instead of silently dropping data. Salvage mode (`mx_store_opts.salvage` or
  `MODX_STORE_SALVAGE=1`) keeps every valid record before and after the damage, keeps the damaged files as
  `*.damaged-<ms>`, writes `recovery-<ms>.txt` and compacts into a clean snapshot. Older journals and
  snapshots open unchanged. Tests cut the journal at every byte offset.
- **Compaction without stopping the world.** Past `compact_threshold` the journal is renamed to
  `journal.old`, a fresh `journal.log` takes new records and a forked child writes `snapshot.json` from its
  copy-on-write image (tmp + fsync + rename + directory fsync). The parent is blocked only for the rename
  and `fork()`. A crash at any point loses nothing: open replays snapshot + `journal.old` + `journal.log`
  and merges them. On macOS `fsync()` is not `F_FULLFSYNC`; the guarantees are those of the platform's
  `fsync()`.
- **Catch-up.** Changes keep their `seq`; clients resume with `Last-Event-ID` or `GET ...?since=` and get
  `410` / `event: reset` when they are older than the retained ring.

## Performance

`make bench` runs `bench/run.mjs` (full profile) against `examples/todo`: a closed-loop load generator in
C (`bench/loadgen.c`), one scenario at a time, 100 000 seeded rows, 64 keep-alive connections, 8 s per
run; SSE fan-out to 1 000 subscribers; a compaction while reads run; a restart that replays the journal.
`bench/compare.sh` builds several revisions in temporary worktrees and benchmarks them back to back with
the same driver, which is how the table below was made. Every number is measured, nothing estimated.

Columns are the commits of each step: **0** the extracted code as it was, **2** group commit, **3**
background compaction, **4** kqueue/epoll, **5** indexes + response cache + gzip (the list endpoint uses
both the `list` index and the seq-invalidated cache), **7** final (timeouts, drain, WebSocket, lingering
close). Results: `bench/results/paired-*.json`. The machine was shared with other work during the run
(load averages below); single runs vary by roughly ±10 % on GET throughput.

| | 0-baseline | 2-group-commit | 3-background-compaction | 4-kqueue | 5-index-cache-gzip | 7-final |
|---|---:|---:|---:|---:|---:|---:|
| GET by id, req/s | 102146 | 102374 | 101106 | 102983 | 100264 | 108351 |
| GET by id, p99 ms | 1.20 | 1.16 | 1.18 | 1.18 | 1.09 | 1.05 |
| list of one list (limit 50), req/s | 110 | 114 | 106 | 110 | 68807 | 71758 |
| list, p99 ms | 587.5 | 574.7 | 612.3 | 642.4 | 1.4 | 1.4 |
| PUT (durable), req/s | 15347 | 45903 | 63195 | 63749 | 57502 | 60987 |
| PUT, p99 ms | 6.17 | 1.56 | 2.87 | 3.16 | 3.57 | 3.48 |
| mixed 80/10/10, req/s | 971 | 908 | 1037 | 987 | 59313 | 61143 |
| mixed, p99 ms | 140.1 | 129.9 | 117.3 | 126.8 | 1.8 | 1.6 |
| GET + 2000 idle conns, req/s | 34843 | 35212 | 34358 | 97059 | 101103 | 100441 |
| GET + 2000 idle conns, p99 ms | 2.65 | 2.97 | 2.61 | 1.20 | 1.14 | 1.17 |
| SSE 1000 subs: delivery p50 ms | 4.4 | 4.5 | 4.4 | 4.2 | 4.4 | 4.3 |
| SSE 1000 subs: delivery p99 ms | 7.3 | 11.2 | 6.4 | 6.6 | 10.1 | 6.7 |
| compaction: loop blocked, ms | 155 | 170 | 6 | 6 | 8 | 7 |
| compaction: worst concurrent read, ms | 156.0 | 170.2 | 6.6 | 6.6 | 8.3 | 7.5 |
| startup from snapshot, ms | 107 | 106 | 103 | 108 | 116 | 118 |
| RSS after start, MiB | 104.4 | 104.4 | 104.4 | 104.5 | 108.9 | 109.0 |
| recovery: journal records replayed per s | 144113 | 163884 | 168832 | 155987 | 148362 | 168276 |

Apple M1 Pro, 10 cores, 16 GiB RAM, darwin 27.2.0, File System Personality: APFS; Protocol: Apple Fabric; Solid State: Yes; load average at start: 4 / 5.8 / 3.4 / 2.8 / 2.8 / 3.4.

CI runs the short profile (`make bench-quick`) on every push and keeps the JSON as an artifact; shared
runners are noisy, so those numbers show trends, not capacity.

## Tests

```sh
make test            # unit tests (store, HTTP parser, JSON, crypto, sessions, OT, WebSocket, gzip, cache, timers)
                     # + tests/todo.mjs against the example, with kqueue/epoll and again with poll()
make test-debug      # the same under ASan + UBSan
make test-tsan       # unit tests under ThreadSanitizer (worker pool, async hashing, socket tests)
make test-install    # install into a temp prefix, build an example with pkg-config and CMake
make fuzz            # mutation fuzzing of the HTTP parser, JSON, journal recovery and WebSocket frames
make fuzz-libfuzzer  # the same targets with libFuzzer (LLVM clang)
```

## Limits

HTTP/1.1 only, no TLS (put it behind a reverse proxy), one event-loop thread per process (plus the small
worker pool for password hashing), request bodies need a
`Content-Length` (chunked uploads get 411), WebSocket without extensions, the store keeps every row in
memory. Defaults are in `http.h` / `ws.h` and can be changed per server.

## License

MIT, © 2026 Saidmashhud Habibzoda. See `LICENSE`.

---

## Кратко по-русски

**modx-core** — небольшой бэкенд-фреймворк на C11: HTTP/1.1-сервер на цикле событий (kqueue / epoll /
poll) с SSE и WebSocket и надёжное хранилище документов в памяти (снапшот + журнал с CRC) с лентой
изменений для живых клиентов. В рантайме нужны только libc/POSIX и системный zlib.

- **Ответ = запись на диске.** Изменения копятся за итерацию цикла, затем один `write()` и один
  `fsync()`, рассылка и только потом ответы (групповая запись).
- **Восстановление.** Обрезается только оборванный хвост журнала. Битая запись в середине, разрыв или
  откат seq, битый снапшот (у него теперь CRC) — открытие падает с понятной ошибкой, а не теряет данные
  молча; режим спасения (`MODX_STORE_SALVAGE=1`) сохраняет всё целое и пишет отчёт.
- **Компакция без остановки мира.** Снапшот пишет дочерний процесс (`fork`, copy-on-write), журнал
  продолжает писаться; падение в любой момент ничего не теряет.
- **Производительность.** Таблица выше: каждая оптимизация измерена отдельно, на одной машине, подряд.
- Приложение регистрирует свои коллекции, ключи, правила видимости и маршруты; пример —
  `examples/todo`. Тесты: `make test`, `make test-debug`, `make fuzz`.

Лицензия MIT.
