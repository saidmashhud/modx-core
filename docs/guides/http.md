# HTTP and routing

The HTTP/1.1 server (`modx/http.h`): the event loop, handlers and responses, routing, limits and timeouts,
listening addresses, deferred responses, the durability barrier and shutdown.

## The loop

One thread runs `http_server_run()`. Each iteration waits for events (kqueue on macOS / BSD, epoll on
Linux, `poll()` elsewhere or with `srv.use_poll = 1`), reads and parses requests, calls the handler for each
complete one, runs `srv.tick`, runs the durability barrier, then writes what is queued. Nothing the
application does on this thread may block: file and socket I/O of the framework is non-blocking, the store's
`fsync` happens once per iteration (group commit), and CPU-heavy work such as password hashing belongs on the
worker pool (see [Sessions and passwords](sessions.md)).

```c
http_server srv;
memset(&srv, 0, sizeof srv);
srv.listen_fd = -1;
if (http_server_listen(&srv, "127.0.0.1", 8080) != 0) { perror("listen"); return 1; }
srv.handler = handle;     /* required */
srv.tick = tick;          /* optional: every iteration (at least once a second) */
srv.ctx = &app;           /* passed to every callback */
http_server_run(&srv, &stop);   /* stop: volatile sig_atomic_t set by a signal handler */
http_server_close(&srv);
```

## Listening addresses

| host | binds |
|---|---|
| `"127.0.0.1"`, `"0.0.0.0"`, any IPv4 literal | IPv4 only |
| `"::1"`, `"[::1]"`, any IPv6 literal | IPv6 only (`IPV6_V6ONLY`) |
| `"::"`, `"*"`, `""`, `NULL` | every address, IPv4 and IPv6 (dual stack); falls back to `0.0.0.0` on a host without IPv6 |
| a name (`"localhost"`) | the first address it resolves to |

Port 0 picks a free port; `srv.port` is the one bound, `srv.listen_family` is `AF_INET` / `AF_INET6`,
`srv.listen_dual_stack` says whether an IPv6 socket also takes IPv4 clients. On a dual-stack socket an IPv4
client arrives as `::ffff:a.b.c.d`; `http_conn.ip` shows it as `a.b.c.d`, so rate-limiter keys and logs are
the same whichever socket a client used. `http_addr_format()` does that formatting for other code.

## Handlers and responses

```c
static void handle(http_server *srv, http_conn *c, const http_request *req, http_response *resp, void *ctx) {
    app *a = ctx;
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/items") == 0) {
        const char *limit = http_req_query(req, "limit");          /* decoded, or NULL */
        sbuf_puts(&resp->body, "[]");                               /* status 200, JSON by default */
        return;
    }
    resp->status = 404;
    sbuf_puts(&resp->body, "{\"error\":\"not found\"}");
}
```

- `http_request`: `method`, `target` (raw), `path` (percent-decoded, no query), `query` / `nquery`
  (decoded), `headers` (names lower-cased; `http_req_header()` first value, `http_req_header_last()` the
  last, e.g. the `X-Forwarded-For` a proxy appended), `body` / `body_len` (NUL-terminated copy; bodies above
  `HTTP_MAX_BODY` are a borrowed view, valid until the handler returns), `keep_alive`.
- `http_response`: `status` (200), `content_type` (`application/json; charset=utf-8`; NULL for none),
  `body` (an `sbuf`), `headers` (extra `"Name: value\r\n"` lines), `file_fd` + `file_len` to stream a file
  (the server takes the descriptor), `sse` / `ws` set by `mx_sse_open()` / `mx_ws_accept()`.
- The server adds `Content-Length` and `Connection`, handles `HEAD`, 204 / 304, keep-alive, pipelining
  and `Expect: 100-continue`.

Routing is plain C on `method` and `path`: compare strings, split the path on `/` when it has parameters.
`examples/todo/todo.c` shows a complete router with ids in the path.

## Static files and gzip

`static_serve(root, req, resp)` serves a file under `root` (call it for GET / HEAD; it reads the whole
file into the body): unsafe paths get 400, `/` and directories map to `index.html`, unknown paths get the
app shell (`app.html`, else `index.html`; SPA fallback) except under `/assets/`, which is 404 when missing
and immutable for a year when present; everything else is `no-cache`. Weak ETag and 304 on
`If-None-Match`. `static_serve_exact()` serves one known file.
Set `srv.gzip_min` (e.g. 1024) to gzip buffered compressible responses of at least that size when the client
accepts it (`gzip_level` 1..9, default 6). Streams and file responses are never compressed.

## Limits and timeouts

| Field (0 = default) | Default | |
|---|---|---|
| `max_conns` | `HTTP_MAX_CONNS` 1024 | further connections are closed at accept |
| `body_limit` callback | `HTTP_MAX_BODY` 1 MiB | per request, after the headers; at most `HTTP_MAX_UPLOAD` 50 MiB |
| `header_timeout_ms` | 10 s | first byte to end of headers (slowloris) |
| `body_timeout_ms` | 60 s | first byte to end of body |
| `write_timeout_ms` | 60 s | output pending without progress |
| `idle_ms` | 60 s | keep-alive connection without a request |
| `drain_ms` | 5 s | graceful shutdown budget |

Headers are limited to `HTTP_MAX_HEADER` (64 KiB, else 431). Requests with `Transfer-Encoding` get 411
(bodies need `Content-Length`). The parser rejects framing ambiguities (bare CR / LF, non-token field names,
obs-fold, control characters, duplicate `Content-Length`, a missing or doubled `Host` in HTTP/1.1) with 400.
After a final error response the server lingers (reads and discards) for `HTTP_LINGER_MS` before closing.

## The durability barrier (group commit)

```c
srv.durable_pending = mx_store_pending_cb;
srv.durable_commit = mx_store_commit_cb;
srv.durable_ctx = store;
```

A response produced while `durable_pending()` is true is held; once per iteration `durable_commit()` makes
everything durable (one write, one fsync) and the held responses go out. Pushes made meanwhile
(`http_conn_send()`, SSE, WebSocket) are queued and sent right after the change notifications the commit
triggers, so a client never hears about a change before it is on disk. If the commit fails the held
responses become 500 (`{"error":{"code":"internal",...}}`), or 503 when it returns
`HTTP_DURABLE_UNAVAILABLE` (the store is read-only after an earlier failure), and the queued pushes are
dropped. See [Store and durability](store.md).

## Deferred responses

A handler that has to wait for work done elsewhere (a worker pool job, an outbound request) defers its
answer instead of blocking:

```c
http_deferred *d = http_defer(c, req, resp);    /* in the handler; resp is ignored from now on */
/* ... later, on the loop thread (e.g. a pool completion): */
http_response *r = http_deferred_response(d);   /* starts as 200, JSON, empty */
r->status = 200;
sbuf_puts(&r->body, "{\"ok\":true}");
http_deferred_finish(d);                         /* exactly once; frees d */
```

- Requests pipelined behind it on the same connection wait, so responses stay in request order. Other
  connections are served normally.
- The idle timeout does not apply while it waits; a graceful drain waits for it (within `drain_ms`).
- `http_deferred_finish()` follows the same barrier rule as a handler's response: if changes are pending
  (e.g. the completion created a session row) it is held until they are durable.
- If the client disconnected meanwhile, the response object is still valid and `finish` just frees it
  (`http_deferred_alive()` says so beforehand; `srv.deferred_orphaned` counts them).
- Not for SSE or WebSocket upgrades.

## Watching other descriptors

`http_server_watch(srv, fd, cb, ctx)` adds a descriptor to the poller; `cb` runs on the loop thread while
it is readable (level-triggered: consume what made it readable). `mx_pool_attach()` uses it for the pool's
wake-up pipe. `http_server_unwatch()` removes it; the descriptor stays open.

## Connection state and shutdown

`c->app` binds application data to a connection (an SSE subscription, a WebSocket session); it is freed
with `srv.app_free` (or `free`) and `srv.on_close` runs first while every connection is still valid.
`http_server_run()` returns after `*stop` became non-zero and the server drained: the listening socket is
closed, SSE streams end, WebSocket connections get a 1001 close, requests that are already arriving are
answered with `Connection: close`, held and deferred responses complete, all within `drain_ms`.
`http_server_close()` runs a last barrier, flushes what it can and frees everything.

See also: [docs index](../README.md) · [API reference](../reference.md) · [Store and durability](store.md) ·
[Server-Sent Events](sse.md) · [WebSocket](websocket.md) · [Sessions and passwords](sessions.md)
