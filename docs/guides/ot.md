# Operational transformation

How to accept concurrent plain-text edits with `modx/ot.h`: the operation model, its wire form, `ot_transform()` and `ot_apply()`, and how a server combines them with revisions.

## What it is for

Several clients edit one text document at the same time. Each sends an operation made against the revision it last saw. The server transforms an operation that is behind against the operations committed since, applies it, and broadcasts the result; every client ends with the same text.

The model is ot.js `TextOperation`. Lengths and offsets count UTF-16 code units, which is what JavaScript strings use, while the text itself stays UTF-8 on the server. A browser client running ot.js can send its operations unchanged.

## Operations

An operation is a sequence of components that spans its base document exactly:

| Component | Wire form | Meaning |
| --- | --- | --- |
| retain | `n` (integer > 0) | keep `n` units |
| insert | `"text"` (non-empty string) | insert the text |
| delete | `{"d": n}` (n > 0) | remove `n` units |

`retains + deletes = base_len`; `retains + inserts = target_len`. An astral code point (emoji, many CJK extension characters) counts 2 units, and a retain or delete may not end inside it.

Example: on `"xy"`, `[1, "A", 1]` gives `"xAy"`; on `"a😀b"`, `[1, {"d": 2}, 1]` gives `"ab"`.

In C:

```c
ot_op op;            /* zero-initialized or ot_init() = empty */
ot_init(&op);
ot_retain(&op, 1);
ot_insert(&op, "A", 1);   /* bytes of UTF-8; copied */
ot_retain(&op, 1);
/* op.base_len == 2, op.target_len == 3 */
ot_free(&op);
```

The builders normalize like ot.js: adjacent components of one kind merge, an insert directly after a delete is moved before it, and zero lengths are ignored. `ot_insert()` silently ignores text that is not valid UTF-8; validate untrusted text with `ot_utf16_len()` (it returns -1) or go through `ot_from_json()`.

## Parsing and serializing

```c
char err[160];
ot_op op;
if (ot_from_json(json_get(body, "ops"), OT_MAX_ITEMS, &op, err, sizeof err) != 0) {
    reply_err(resp, 400, "bad_ops"); /* err says which item and why */
    return;
}
json *wire = ot_to_json(&op);      /* new array; caller frees */
```

`ot_from_json()` initializes `op`, normalizes, and rejects anything outside the wire form: non-integral or non-positive numbers, empty strings, strings with NUL or invalid UTF-8, objects other than `{"d": n}`, more than `max_items` items. On error `op` is empty. It is stricter than the ot.js client, which drops empty components. `ot_is_noop(&op)` is 1 when the operation only retains.

## Apply

```c
sbuf out;
sbuf_init(&out);
if (ot_apply(text, text_len, &op, &out, err, sizeof err) != 0) {
    sbuf_free(&out);
    /* span does not match the text, a boundary splits a surrogate pair, or text is not UTF-8 */
}
```

`ot_apply()` appends the result to `out`; with `out` NULL it only validates. On failure `out` may already hold part of the result, so discard it.

## Transform

```c
ot_op a2, b2;
if (ot_transform(&a, &b, &a2, &b2) != 0) { /* base lengths differ */ }
/* apply(apply(S, a), b2) == apply(apply(S, b), a2) */
ot_free(&a2);
ot_free(&b2);
```

`a` and `b` are concurrent operations on the same base. `a2` is `a` rewritten to apply after `b`, `b2` is `b` rewritten to apply after `a`. Either output may be NULL when it is not needed; both are initialized by the call, so pass them empty, and both are empty on failure. When both insert at the same position, `a`'s insert ends up first. Overlapping deletes remove the span once; an insert inside a span the other operation deletes survives.

There is no `compose`: the library does not merge consecutive operations into one. A server keeps each committed operation and transforms against them one by one.

## Server with revisions

The document is its text plus a revision counter; `hist[r - base_rev]` is the operation that took it from revision `r` to `r + 1`. A client submits `{"rev": r, "ops": [...]}`.

```c
typedef struct {
    sbuf text;         /* UTF-8 */
    int64_t rev;       /* current revision */
    int64_t base_rev;  /* oldest revision hist still covers */
    ot_op *hist;       /* rev - base_rev operations */
} doc;

/* 0 = applied (op is now the operation to broadcast), 1 = rev too old, -1 = invalid */
static int submit(doc *d, int64_t rev, ot_op *op, char *err, size_t errlen) {
    if (rev < d->base_rev || rev > d->rev) return 1; /* client re-fetches the document */
    for (int64_t r = rev; r < d->rev; r++) {
        ot_op t;
        if (ot_transform(op, &d->hist[r - d->base_rev], &t, NULL) != 0) {
            snprintf(err, errlen, "ops do not span revision %lld", (long long)rev);
            return -1;
        }
        ot_free(op);
        *op = t;
    }
    if (op->target_len > OT_MAX_TEXT) { snprintf(err, errlen, "document too long"); return -1; }
    sbuf out;
    sbuf_init(&out);
    if (ot_apply(d->text.data ? d->text.data : "", d->text.len, op, &out, err, errlen) != 0) {
        sbuf_free(&out);
        return -1;
    }
    sbuf_free(&d->text);
    d->text = out;
    /* append *op to hist (moving ownership), drop the oldest beyond a fixed window, d->rev++ */
    return 0;
}
```

The incoming operation is the first argument of `ot_transform()`, as in the ot.js server (`transform(operation, concurrent)[0]`). On a tie the client whose operation arrives later therefore gets its insert first; the ot.js client makes the same choice when it transforms its pending operation against one from the server, so both sides converge.

After a successful submit, persist the text and revision (for example as a store row, so the durability barrier covers it) and send the transformed operation with its new revision to every other client (SSE or WebSocket), and an acknowledgement to the sender. Clients that fall behind the history window get the full document again.

## Limits

- `OT_MAX_TEXT` (1 000 000 UTF-16 units) and `OT_MAX_ITEMS` (10 000 components) are values for the application to enforce: pass `OT_MAX_ITEMS` to `ot_from_json()` and check `target_len` against `OT_MAX_TEXT` yourself. `ot_apply()` itself has no length cap.
- Lengths are `int64_t`; components are heap-allocated and owned by the `ot_op`. Free every operation with `ot_free()`, including the outputs of `ot_transform()`.
- JSON escapes for lone surrogates (`"\ud83d"`) are decoded by the JSON parser to U+FFFD, which counts one unit, the same as the lone unit in JavaScript.

See also: [reference](../reference.md), [README](../README.md), [HTTP server](http.md), [store](store.md), [SSE](sse.md), [WebSocket](websocket.md), [sessions](sessions.md), [idempotency](idempotency.md)
