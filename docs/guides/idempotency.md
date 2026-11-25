# Idempotency keys

How to make retried writes safe with `modx/idem.h`: remember the first completed answer for an `Idempotency-Key` and replay it instead of executing the write again.

## What it is for

A client whose write timed out cannot tell whether the server executed it. If it sends the same `Idempotency-Key: <key>` header with every attempt, the server can execute the write once and answer later attempts with the stored response.

The module is the mechanism. The application chooses the policy:

- where the records live: a collection of its store, declared `MX_COLL_PRIVATE | MX_COLL_NO_RING` (persisted, never broadcast as a public change, no slot in the change ring);
- the record id, which is the scope of a key (for example `"<account>|<key>"`, so two accounts cannot see each other's answers);
- retention: by age (`mx_idem_prune()`) and by count per owner (`mx_idem_cap()`).

## Records

A record is a JSON row:

```
{id, method, target, status, content_type, body | body_omitted:true, created_at, ...}
```

`target` is the raw request target (path and query). The application may add fields, such as an `owner` used for the per-owner cap.

## Setup

```c
#include "modx/idem.h"

enum { C_TODOS, C_IDEM, C_NCOLL };
static const char *const TODO_INDEX[] = { "list", NULL };
static const char *const IDEM_INDEX[] = { "owner", NULL };
static const mx_coll_def COLLS[C_NCOLL] = {
    { "todos", 0, TODO_INDEX },
    { "idempotency_keys", MX_COLL_PRIVATE | MX_COLL_NO_RING, IDEM_INDEX },
};

#define IDEM_TTL_MS (24LL * 3600 * 1000) /* replay window */
#define IDEM_MAX_BODY (64 * 1024)        /* larger bodies are not stored */
#define IDEM_PER_OWNER 1000              /* records kept per owner */
```

The `owner` index makes `mx_idem_cap()` a lookup instead of a scan.

## Handler

Wrap the write routes. `do_write()` stands for the application's own routing (`create_todo()` and friends in `examples/todo/todo.c`); `owner` is whatever identifies the caller.

```c
static void idempotent_write(app *a, const http_request *req, http_response *resp, const char *owner) {
    const char *key = http_req_header(req, MX_IDEM_HEADER);
    if (!key || !mx_idem_method(req->method)) { do_write(a, req, resp); return; }
    if (!mx_idem_key_valid(key)) { reply_err(resp, 400, "bad_idempotency_key"); return; }

    sbuf id;
    sbuf_init(&id);
    sbuf_printf(&id, "%s|%s", owner, key);
    int64_t now = now_ms();
    const json *rec = mx_store_get(a->st, C_IDEM, id.data);
    if (rec && mx_idem_expired(rec, now, IDEM_TTL_MS)) rec = NULL; /* treat as absent */
    if (rec && !mx_idem_matches(rec, req)) {
        reply_err(resp, 422, "idempotency_mismatch");
    } else if (rec) {
        mx_idem_replay(rec, resp);
    } else {
        do_write(a, req, resp);
        if (mx_idem_storable(resp) && mx_idem_cap(a->st, C_IDEM, "owner", owner, IDEM_PER_OWNER) >= 0) {
            json *r = mx_idem_record(id.data, req, resp, now, IDEM_MAX_BODY);
            json_set(r, "owner", json_str(owner));
            mx_store_put(a->st, C_IDEM, r, NULL); /* takes ownership of r */
        }
    }
    sbuf_free(&id);
}
```

The record is put in the same handler call as the write, so both are in the same group commit: with the durability barrier configured, one fsync makes the write and its record durable together, or the commit fails and the held response becomes a 500 / 503, in which case neither is. A failed `mx_store_put()` of the record means the store has failed, which the barrier reports the same way.

An expired record is simply overwritten by the new one (same id).

## Functions

| Function | Behavior |
| --- | --- |
| `mx_idem_key_valid(key)` | 1 for 1..`MX_IDEM_KEY_MAX` (128) bytes of printable ASCII (0x20..0x7e) that do not start or end with a space. NULL is invalid. |
| `mx_idem_method(method)` | 1 for `POST`, `PUT`, `PATCH`, `DELETE` (exact, upper case). Other methods ignore the header. |
| `mx_idem_storable(resp)` | 1 for 2xx and 4xx except 408 and 429, when the response is buffered. 5xx, 1xx / 3xx, SSE, WebSocket and file responses are never stored, so the client can retry them. |
| `mx_idem_record(id, req, resp, now, max_body)` | New record row; caller owns it. A body above `max_body`, containing NUL, or not valid UTF-8 is replaced by `body_omitted:true`. |
| `mx_idem_matches(rec, req)` | 1 when method and raw target (path + query) equal the record's. The request body is not compared. |
| `mx_idem_expired(rec, now, ttl_ms)` | 1 when `created_at + ttl_ms <= now`, or the record has no numeric `created_at`. |
| `mx_idem_replay(rec, resp)` | Sets status, content type and body from the record and appends the `Idempotent-Replayed: true` header (`MX_IDEM_REPLAYED_HEADER`). |
| `mx_idem_prune(st, coll, cutoff)` | Deletes records created before `cutoff` and records without a numeric `created_at`. Returns the count, or -1 when the store refused a delete. |
| `mx_idem_cap(st, coll, field, value, max)` | Deletes the oldest records (by `created_at`, then id) of the owner whose `field` equals `value` until fewer than `max` remain, making room for one more. Returns the count or -1; `value` NULL does nothing. |

`MX_IDEM_HEADER` is `"idempotency-key"`, lower-cased as `http_req_header()` expects.

## Replay details

- The replay has the original status and content type. If the body was omitted, the replay answers the original status with `{"replayed":true,"body_omitted":true}` and a JSON content type.
- `resp->content_type` may point into the record, so the record must stay in the store until the response is serialized. That holds when replaying from the handler, as above.
- Extra headers of the original response (`Location`, cookies) are not stored; only status, content type and body are.
- The 422 for a reused key with another method or target is the application's choice; the module only reports the mismatch.

## Retention

Prune by age from the tick. The deletes are journaled; with a durability barrier they are committed in the same loop iteration, otherwise call `mx_store_sync()` after.

```c
static void tick(http_server *srv, void *ctx) {
    (void)srv;
    app *a = ctx;
    int64_t now = now_ms();
    if (now - a->last_prune > 60000) {
        a->last_prune = now;
        if (mx_idem_prune(a->st, C_IDEM, now - IDEM_TTL_MS) < 0) { /* store failed: mx_store_failed() */ }
    }
    mx_store_tick(a->st);
}
```

`mx_idem_prune()` scans the whole collection; `mx_idem_cap()` uses the index when the field is declared. Records survive restarts and compaction like any other row.

## Concurrency

Handlers run one at a time on the loop thread and `mx_store_put()` applies in memory immediately, so a duplicate request arriving right after the first sees the record even before the commit (its replay is held by the barrier like any response). A handler that answers later through `http_defer()` has no record yet while the work runs; such routes need their own in-flight marker if duplicates must not start the work twice.

See also: [reference](../reference.md), [README](../README.md), [HTTP server](http.md), [store](store.md), [sessions](sessions.md), [SSE](sse.md), [WebSocket](websocket.md), [OT](ot.md)
