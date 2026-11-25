# Server-Sent Events

How to turn a handler's response into an event stream with `modx/sse.h`, fan events out, and let reconnecting clients catch up.

## What it is

An SSE stream is an ordinary `GET` whose response never ends: the server writes `text/event-stream` events on the same connection for as long as the client stays. The hub (`mx_sse`) knows every open stream of one `http_server`, sends events to all of them or to a filtered subset, and writes comment pings so idle proxies do not drop the connection.

Everything runs on the server's loop thread. There is no locking and no background thread.

## Minimal server

Three pieces: a hub initialized after `http_server_listen()`, a route that opens the stream, and a tick that pings.

```c
#include "modx/http.h"
#include "modx/sse.h"
#include "modx/store.h"

typedef struct {
    mx_store *st;
    mx_sse sse;
} app;

static void handle(http_server *srv, http_conn *c, const http_request *req, http_response *resp, void *ctx) {
    (void)srv;
    (void)ctx;
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/events") == 0) {
        mx_sse_open(c, resp, NULL); /* NULL: no per-connection data */
        return;
    }
    /* ... other routes ... */
}

static void tick(http_server *srv, void *ctx) {
    (void)srv;
    app *a = ctx;
    mx_sse_tick(&a->sse); /* ": ping" every ping_ms */
}

/* in main(), after http_server_listen(&srv, host, port): */
mx_sse_init(&a.sse, &srv, 15000); /* ping_ms <= 0 means 15000 */
srv.handler = handle;
srv.tick = tick;
srv.ctx = &a;
```

`mx_sse_open()` sets `resp->sse`, binds `app` to `c->app` when it is not NULL, and writes `retry: 2000` (the client's reconnect delay) into the body. Anything else the handler appends to `resp->body` afterwards goes out as the first chunk of the stream; that is where catch-up events go.

## Sending events

```c
/* to every stream */
mx_sse_broadcast(&a->sse, NULL, NULL, "todo", ch->seq, ch->json);
/* to one stream */
mx_sse_send(c, NULL, -1, "{\"hello\":true}");
```

The arguments are `(event, id, data)`:

| Argument | Effect |
| --- | --- |
| `event` | `event: <event>` line; NULL omits it (the client sees a `message` event). |
| `id` | `id: <id>` line when `id >= 0`; pass -1 for none. |
| `data` | One `data:` line per line of `data` (split on `\n`); NULL sends an empty `data:` line. |

`mx_sse_broadcast()` formats the event once and returns the number of streams it was queued on. `mx_sse_format(sbuf *out, ...)` appends the same encoding to a buffer without sending it; use it to build catch-up bodies. `mx_sse_count()` returns the number of live streams.

### Filtering

A filter decides per connection. Bind the subscription to the connection with the `app` argument of `mx_sse_open()` and read it back from `c->app`:

```c
typedef struct { char list[64]; } sub;

static int same_list(void *ctx, http_conn *c) {
    const sub *s = c->app;
    return s && strcmp(s->list, ctx) == 0;
}

/* handler */
sub *s = calloc(1, sizeof *s);
snprintf(s->list, sizeof s->list, "%s", list);
mx_sse_open(c, resp, s);

/* push */
mx_sse_broadcast(&a->sse, same_list, (void *)list, NULL, ch->seq, ch->json);
```

`c->app` is freed with `http_server.app_free` (NULL means `free()`) when the connection closes. `http_server.on_close` runs before that for every connection that has `app` set, except on shutdown.

## Catch-up with Last-Event-ID

When events carry the store's change `seq` as their id, a browser's `EventSource` sends `Last-Event-ID: <seq>` when it reconnects. The handler replays what the client missed from the store's change ring into the first chunk. This is `open_events()` from `examples/todo/todo.c`:

```c
static void replay_change(void *ctx, const mx_change *ch) { mx_sse_format(ctx, NULL, ch->seq, ch->json); }

static void open_events(app *a, http_conn *c, const http_request *req, http_response *resp) {
    mx_sse_open(c, resp, NULL);
    const char *last = http_req_header(req, "last-event-id");
    if (last && *last) {
        long long since = strtoll(last, NULL, 10);
        if (mx_store_for_each_since(a->st, since, NULL, NULL, replay_change, &resp->body) != 0)
            sbuf_printf(&resp->body, "event: reset\ndata: {\"seq\":%lld}\n\n", (long long)a->st->seq);
    }
}
```

`mx_store_for_each_since(st, since, keep, keep_ctx, fn, ctx)` visits every durable retained public change with `seq > since` and returns -1 when `since` is older than the ring holds or ahead of the store's `seq` (the data directory was reset). The stream cannot then be repaired event by event; the example sends a `reset` event and the client re-bootstraps. The same function backs a polling endpoint, which answers `410 Gone` in that case (`/api/changes` in the example). The ring size is the `ring_cap` argument of `mx_store_open()`; changes of `MX_COLL_PRIVATE` or `MX_COLL_NO_RING` collections are never in it. Pass a `keep` filter to replay only what this client may see.

Live events come from the store listener, which runs only for durable changes:

```c
static void on_durable(void *ctx, const mx_change *ch) {
    app *a = ctx;
    mx_sse_broadcast(&a->sse, NULL, NULL, NULL, ch->seq, ch->json);
}

a.st->listener = on_durable;
a.st->listener_ctx = &a;
```

## Guarantees and limits

- **Durability barrier.** Pushes go through `http_conn_send()`. With `durable_pending` / `durable_commit` set on the server, bytes pushed while changes are uncommitted wait for the commit and go out after the change notifications it triggers, and are dropped if the commit fails. A client is never told about something that is not on disk. The stream's opening response is not held by the barrier, because its catch-up only contains durable changes.
- **Never gzipped.** The opening chunk and every later event share one connection, so `gzip_min` does not apply to streams.
- **Headers.** The server writes `200`, `Content-Type: text/event-stream`, `Cache-Control: no-cache`, `Connection: keep-alive` and `X-Accel-Buffering: no` (nginx buffering off), plus any lines the handler put in `resp->headers`.
- **Pings.** `mx_sse_tick()` writes `: ping` to every stream once `ping_ms` has passed since the last round. Call it from `http_server.tick`.
- **Timeouts.** Idle and header timeouts do not apply to streams. The write timeout does: a stream whose output makes no progress for `write_timeout_ms` (default `HTTP_WRITE_TIMEOUT_MS`, 60 s) is dropped.
- **Backlog.** A connection with more than `HTTP_SSE_MAX_BACKLOG` (8 MiB) unsent is marked dead; the client reconnects and catches up.
- **Input.** Bytes a client sends on a stream are discarded.
- **Shutdown.** `http_server_run()` closes streams when it drains; clients reconnect elsewhere or later.
- **Ids.** Event ids are whatever the application passes. Using the store's `seq` makes `Last-Event-ID` and `mx_store_for_each_since()` line up.

See also: [reference](../reference.md), [README](../README.md), [HTTP server](http.md), [WebSocket](websocket.md), [store](store.md), [sessions](sessions.md), [idempotency](idempotency.md), [OT](ot.md)
