# WebSocket

How to accept WebSocket (RFC 6455) connections with `modx/ws.h`, receive messages, send and broadcast frames, and what the server enforces.

## What it is

WebSocket runs on the same `http_server` and event loop as plain HTTP and SSE. A handler accepts the upgrade with `mx_ws_accept()`; from the `101` on, the connection carries frames. The server reassembles fragmented messages, answers pings, handles the closing handshake, pings idle sockets and drops ones that stop answering. The application sees complete text or binary messages through one callback, `http_server.on_ws_message`.

Not supported: extensions (no `permessage-deflate`) and subprotocol negotiation beyond echoing one value the application picks.

## Minimal server

From `examples/todo/todo.c`: accept on `/api/ws`, echo every message back.

```c
#include "modx/http.h"
#include "modx/ws.h"

static void handle(http_server *srv, http_conn *c, const http_request *req, http_response *resp, void *ctx) {
    (void)srv;
    (void)ctx;
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/ws") == 0) {
        int st = mx_ws_accept(c, req, resp, NULL, NULL);
        if (st) reply_err(resp, st, st == 426 ? "upgrade_required" : "bad_handshake");
        return;
    }
    /* ... other routes ... */
}

static void on_ws_message(http_server *srv, http_conn *c, int opcode, const char *data, size_t len, void *ctx) {
    (void)srv;
    (void)ctx;
    mx_ws_send(c, opcode, data, len); /* echo with the same opcode */
}

/* in main() */
srv.handler = handle;
srv.on_ws_message = on_ws_message;
srv.ws_max_message = 0; /* 0 = MX_WS_MAX_MESSAGE (1 MiB) */
srv.ws_ping_ms = 0;     /* 0 = MX_WS_PING_MS (30 s) */
```

`reply_err()` is the example's helper that sets the status and a JSON error body.

## Handshake

```c
int mx_ws_accept(http_conn *c, const http_request *req, http_response *resp, void *app, const char *subprotocol);
```

| Return | Meaning | `resp` |
| --- | --- | --- |
| 0 | Accepted. | Turned into `101 Switching Protocols` with `Upgrade`, `Connection` and `Sec-WebSocket-Accept`; `resp->ws` is set. |
| 400 | Not a `GET`, no `Upgrade: websocket` + `Connection: upgrade` tokens, or `Sec-WebSocket-Key` is not 24 base64 characters decoding to 16 bytes. | Untouched. |
| 426 | `Sec-WebSocket-Version` is missing or not `13`. | Only a `Sec-WebSocket-Version: 13` header line added. |

On a non-zero return the caller sets the status itself (as above). `app`, when not NULL, is bound to `c->app` exactly like `mx_sse_open()` does, and freed with `http_server.app_free` when the connection closes. `subprotocol` is echoed as `Sec-WebSocket-Protocol`; to negotiate, read `http_req_header(req, "sec-websocket-protocol")`, pick one of the offered values and pass it.

`mx_ws_is_upgrade(req)` tests the two upgrade headers without accepting, which lets a route serve both a normal `GET` and an upgrade. `mx_ws_accept_key(key, out)` computes `Sec-WebSocket-Accept` (29 bytes including the NUL).

## Receiving

`on_ws_message(srv, c, opcode, data, len, ctx)` is called once per complete message, with `opcode` `MX_WS_TEXT` or `MX_WS_BINARY` and `ctx` = `http_server.ctx`. `data` is valid only during the call, is never NULL, and is followed by a NUL byte; binary payloads may contain NULs, so use `len`. Text messages have already been checked to be valid UTF-8. `srv->ws_messages` counts messages received.

Control frames never reach the callback:

- A ping is answered with a pong carrying the same payload.
- A pong is ignored (any frame from the peer counts as an answer to the server's ping).
- A close frame is echoed with the same status code (no status if the peer sent none), then the TCP connection closes once the echo is written.

Per-connection state belongs in `c->app` (set through `mx_ws_accept()`).

## Sending

```c
mx_ws_send(c, MX_WS_BINARY, buf, n);           /* one unfragmented frame */
mx_ws_send_text(c, "{\"type\":\"hello\"}");    /* NUL-terminated text */
mx_ws_close(c, 1000, "bye");                   /* closing handshake */
size_t n = mx_ws_broadcast(srv, keep, kctx, MX_WS_TEXT, ch->json, strlen(ch->json));
```

- Server frames are never masked or fragmented. `mx_ws_send()` is a no-op unless `c` is an open WebSocket that is not closing, so it is safe to call on a connection that is going away.
- `mx_ws_close(c, code, reason)`: `code` 0 sends a close frame without a status; `reason` is truncated to 123 bytes. The connection closes as soon as the frame is written; the server does not wait for the peer's close.
- `mx_ws_broadcast()` encodes the frame once, sends it to every open, non-closing WebSocket that `keep(ctx, c)` accepts (`keep` NULL = all), and returns how many it reached.

The todo example pushes every durable store change to all sockets from the store listener:

```c
static void on_durable(void *ctx, const mx_change *ch) {
    app *a = ctx;
    mx_sse_broadcast(&a->sse, NULL, NULL, NULL, ch->seq, ch->json);
    mx_ws_broadcast(a->sse.srv, NULL, NULL, MX_WS_TEXT, ch->json, strlen(ch->json));
}
```

Sends go through `http_conn_send()`, so they respect the durability barrier the same way SSE pushes do: bytes pushed while changes are uncommitted wait for the commit and are dropped if it fails.

## Limits and close codes

| Code | Sent when |
| --- | --- |
| 1002 | Protocol error: an unmasked client frame, RSV bits set, an unknown opcode, a fragmented or over-125-byte control frame, a non-minimal length encoding, a new data frame while a fragmented message is open, a continuation with no message, a malformed close frame (1-byte payload, invalid status code, reason not UTF-8). |
| 1007 | A text message that is not valid UTF-8 (checked on the reassembled message). |
| 1009 | A frame or reassembled message above `ws_max_message` (default `MX_WS_MAX_MESSAGE`, 1 MiB). |
| 1001 | Graceful shutdown (`"server shutting down"`). |

After a 1002 / 1007 / 1009 the server discards the remaining input and closes once the close frame is written.

Liveness and backpressure:

- **Ping.** A socket with no input for `ws_ping_ms` (default `MX_WS_PING_MS`, 30 s) gets a ping. If no frame arrives within another `ws_ping_ms`, the connection is dropped without a close frame and counted in `srv->timeouts`.
- **Idle and header timeouts** do not apply after the upgrade; the ping replaces them.
- **Write timeout.** Output that makes no progress for `write_timeout_ms` (default 60 s) drops the connection.
- **Backlog.** More than `HTTP_SSE_MAX_BACKLOG` (8 MiB) unsent bytes on a connection marks it dead; slow readers are cut rather than buffered without bound.

The frame parser (`mx_ws_parse_frame()`) and the UTF-8 validator (`mx_utf8_valid()`) are exported for tests and fuzzing.

See also: [reference](../reference.md), [README](../README.md), [HTTP server](http.md), [SSE](sse.md), [store](store.md), [sessions](sessions.md), [idempotency](idempotency.md), [OT](ot.md)
