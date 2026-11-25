# Getting started

Build and test the framework, install it or vendor it, link an application against it, and read through a
minimal server.

## Requirements

A C11 compiler (clang or GCC), make, POSIX threads, and the zlib headers (`zlib1g-dev` on Debian / Ubuntu;
macOS ships zlib). Node.js 18 or newer only for the end-to-end tests. Linux (epoll), macOS and the BSDs
(kqueue); anything else with `poll()`.

## Build and test

```sh
make                # build/libmodxcore.a, build/todo, build/minimal
make test           # unit tests + examples/todo end to end (kqueue/epoll, then poll())
make test-debug     # the same under AddressSanitizer + UndefinedBehaviorSanitizer
make test-tsan      # unit tests under ThreadSanitizer (worker pool, async hashing, socket tests)
make fuzz           # mutation fuzzing: HTTP parser, JSON, journal recovery, WebSocket frames
make test-install   # install into a temp prefix and build an example with pkg-config and CMake
make docs           # API reference with Doxygen (skipped when doxygen is missing)
```

Build options: `ZLIB=0` leaves gzip out (`-DMODX_NO_ZLIB`, no `-lz`), `CFLAGS=-DMODX_POLLER_POLL` forces the
`poll()` poller, `BUILD=dir` picks the object directory, `CC=gcc` the compiler.

## Install

```sh
make install PREFIX=/usr/local      # or PREFIX=$HOME/.local; DESTDIR=/staging for packages
make uninstall PREFIX=/usr/local
```

This installs:

| Path | |
|---|---|
| `include/modx/*.h` | the headers; include them as `#include "modx/http.h"` |
| `lib/libmodx.a` | static library |
| `lib/libmodx.so.0.3.0`, `libmodx.so.0.3`, `libmodx.so` | shared library (soname `libmodx.so.0.3`); on macOS `libmodx.0.3.0.dylib` (install name `libmodx.0.3.dylib`). `SHARED=0` skips it |
| `lib/pkgconfig/modx-core.pc` | pkg-config |
| `lib/cmake/modx-core/` | CMake package: `find_package(modx-core)` gives `modx::core` |

The ABI version is `0.<minor>` while the major version is 0: every minor release may change structures.

## Link an application

pkg-config:

```sh
cc -std=c11 $(pkg-config --cflags modx-core) -o app app.c $(pkg-config --libs modx-core)
# static: the archive and its dependencies
cc -std=c11 $(pkg-config --cflags modx-core) -o app app.c /usr/local/lib/libmodx.a -pthread -lz -lm
```

CMake, against an installed copy:

```cmake
find_package(modx-core 0.3 REQUIRED)
target_link_libraries(app PRIVATE modx::core)
```

CMake, from source (a submodule or `FetchContent`):

```cmake
add_subdirectory(third_party/modx-core)          # or:
include(FetchContent)
FetchContent_Declare(modx-core GIT_REPOSITORY https://github.com/saidmashhud/modx-core.git GIT_TAG main)
FetchContent_MakeAvailable(modx-core)
target_link_libraries(app PRIVATE modx::core)
```

The top-level `CMakeLists.txt` takes `-DMODX_ZLIB=OFF`, `-DBUILD_SHARED_LIBS=ON`, and builds the unit
tests (`ctest`) only when it is the top-level project. The Makefile stays the primary build; both compile
the same sources.

Without a build system: copy `include/` and `src/` into your tree and compile every `src/*.c` with
`-std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -pthread -Iinclude`; link with `-pthread -lz -lm`.
`examples/install/` is a consumer project used by `make test-install`.

## A minimal server

`examples/minimal/minimal.c` (built by `make` as `build/minimal`) is a complete server in about 170 lines:
a notes collection, a list and a create endpoint, Server-Sent Events for every durable change, and a login
whose password check runs on the worker pool.

```sh
MINIMAL_PORT=8080 ./build/minimal &
curl -X POST -d '{"text":"hello"}' http://127.0.0.1:8080/api/notes
curl http://127.0.0.1:8080/api/notes
curl -N http://127.0.0.1:8080/api/events             # in another terminal: one event per change
curl -X POST -d '{"password":"demo-password"}' http://127.0.0.1:8080/api/login
```

The pieces, in the order `main()` sets them up:

1. **The store.** Collections are declared by the application; rows are JSON objects with a string `"id"`.

   ```c
   enum { C_NOTES, C_NCOLL };
   static const mx_coll_def COLLS[C_NCOLL] = { { "notes", 0, NULL } };
   a.st = mx_store_open(dir, COLLS, C_NCOLL, 1000, NULL, err, sizeof err);  /* 1000 = change ring size */
   ```

   If the open fails, `err` says why. A torn journal tail from a crash is repaired automatically; other
   damage stops the open instead of silently dropping data (see [the store guide](guides/store.md)).

2. **The worker pool** for password hashing, which would otherwise block every connection for about half a
   second per login: `a.pool = mx_pool_new(2, 0);` and, once the server exists,
   `mx_pool_attach(a.pool, &srv);` so completions run on the loop thread.

3. **The server.** `http_server_listen(&srv, host, port)` with `"127.0.0.1"`, `"::1"`, `"::"` (IPv4 and IPv6)
   or `"0.0.0.0"`; port 0 picks a free port (`srv.port`).

4. **Group commit.** With these three fields a response produced while changes are pending waits until one
   `write()` + `fsync()` per loop iteration made them durable:

   ```c
   srv.durable_pending = mx_store_pending_cb;
   srv.durable_commit = mx_store_commit_cb;
   srv.durable_ctx = a.st;
   ```

   So the `201` of `POST /api/notes` is sent only once the note is on disk. If the disk fails, held
   responses become `500` and the store turns read-only (later writes: `503`).

5. **Fan-out.** The store's listener runs after each fsync, in seq order; the example broadcasts every
   public change to every SSE stream: `mx_sse_broadcast(&a->sse, NULL, NULL, NULL, ch->seq, ch->json);`.

6. **The handler** routes on `req->method` and `req->path`, fills `resp` (`status` defaults to 200, the
   body is an `sbuf`, the content type JSON) and returns. Nothing in a handler may block.

7. **A deferred response.** The login handler calls `http_defer()`, queues
   `password_verify_async()`, and returns. The loop keeps serving other connections; when the worker is
   done, `login_done()` runs on the loop thread, fills `http_deferred_response()` and calls
   `http_deferred_finish()`. Requests pipelined behind it on the same connection wait, so answers stay in
   order.

8. **Run and shut down.** `http_server_run(&srv, &g_stop)` returns after SIGINT / SIGTERM once it has
   drained (requests in flight answered, held responses committed). Then close the server, free the pool
   (it runs the last completions), sync and close the store.

## Next

- [HTTP and routing](guides/http.md), [Store and durability](guides/store.md),
  [Sessions and passwords](guides/sessions.md).
- `examples/todo/todo.c`: a larger server (indexes, response cache, gzip, WebSocket, static files, catch-up
  with `Last-Event-ID`) and its end-to-end test `tests/todo.mjs`.

See also: [docs index](README.md) · [API reference](reference.md)
