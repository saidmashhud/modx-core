#include "modx/ot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ot_init(ot_op *op) { memset(op, 0, sizeof *op); }

void ot_free(ot_op *op) {
    for (size_t i = 0; i < op->len; i++) free(op->c[i].s);
    free(op->c);
    ot_init(op);
}

static ot_comp *push(ot_op *op) {
    if (op->len == op->cap) {
        op->cap = op->cap ? op->cap * 2 : 8;
        op->c = xrealloc(op->c, op->cap * sizeof *op->c);
    }
    ot_comp *c = &op->c[op->len++];
    memset(c, 0, sizeof *c);
    return c;
}

static ot_comp *last(ot_op *op) { return op->len ? &op->c[op->len - 1] : NULL; }

void ot_retain(ot_op *op, int64_t n) {
    if (n <= 0) return;
    op->base_len += n;
    op->target_len += n;
    ot_comp *l = last(op);
    if (l && l->kind == OT_RETAIN) { l->n += n; return; }
    ot_comp *c = push(op);
    c->kind = OT_RETAIN;
    c->n = n;
}

static void append_str(ot_comp *c, const char *s, size_t slen, int64_t n) {
    c->s = xrealloc(c->s, c->slen + slen + 1);
    memcpy(c->s + c->slen, s, slen);
    c->slen += slen;
    c->s[c->slen] = 0;
    c->n += n;
}

void ot_insert(ot_op *op, const char *s, size_t slen) {
    if (!slen) return;
    int64_t n = ot_utf16_len(s, slen);
    if (n <= 0) return; /* callers pass validated text */
    op->target_len += n;
    ot_comp *l = last(op);
    if (l && l->kind == OT_INSERT) { append_str(l, s, slen, n); return; }
    if (l && l->kind == OT_DELETE) {
        /* canonical order: an insert never directly follows a delete */
        ot_comp *before = op->len >= 2 ? &op->c[op->len - 2] : NULL;
        if (before && before->kind == OT_INSERT) { append_str(before, s, slen, n); return; }
        ot_comp del = *l;
        memset(l, 0, sizeof *l);
        l->kind = OT_INSERT;
        append_str(l, s, slen, n);
        *push(op) = del;
        return;
    }
    ot_comp *c = push(op);
    c->kind = OT_INSERT;
    append_str(c, s, slen, n);
}

void ot_delete(ot_op *op, int64_t n) {
    if (n <= 0) return;
    op->base_len += n;
    ot_comp *l = last(op);
    if (l && l->kind == OT_DELETE) { l->n += n; return; }
    ot_comp *c = push(op);
    c->kind = OT_DELETE;
    c->n = n;
}

/* Strict UTF-8 decoder: bytes consumed (0 = invalid), code point in *cp. */
static size_t dec(const unsigned char *p, size_t n, unsigned *cp) {
    if (!n) return 0;
    unsigned c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    size_t len;
    unsigned v, min;
    if ((c & 0xE0) == 0xC0) { len = 2; v = c & 0x1F; min = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0F; min = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07; min = 0x10000; }
    else return 0;
    if (n < len) return 0;
    for (size_t i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (p[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    *cp = v;
    return len;
}

int64_t ot_utf16_len(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    int64_t units = 0;
    size_t i = 0;
    while (i < n) {
        unsigned cp;
        size_t k = dec(p + i, n - i, &cp);
        if (!k) return -1;
        units += cp >= 0x10000 ? 2 : 1;
        i += k;
    }
    return units;
}

int ot_from_json(const json *arr, size_t max_items, ot_op *op, char *err, size_t errlen) {
    ot_init(op);
    if (!arr || arr->type != JSON_ARRAY) { snprintf(err, errlen, "ops must be an array"); return -1; }
    if (arr->len > max_items) { snprintf(err, errlen, "ops has more than %zu items", max_items); return -1; }
    for (size_t i = 0; i < arr->len; i++) {
        const json *it = arr->items[i];
        if (it->type == JSON_NUMBER) {
            if (!json_is_int(it) || it->number <= 0) {
                snprintf(err, errlen, "ops[%zu]: a retain must be a positive integer", i);
                goto bad;
            }
            ot_retain(op, (int64_t)it->number);
        } else if (it->type == JSON_STRING) {
            if (!it->str_len || strlen(it->str) != it->str_len) {
                snprintf(err, errlen, "ops[%zu]: an insert must be a non-empty string without NUL characters", i);
                goto bad;
            }
            if (ot_utf16_len(it->str, it->str_len) < 0) {
                snprintf(err, errlen, "ops[%zu]: an insert must be valid UTF-8", i);
                goto bad;
            }
            ot_insert(op, it->str, it->str_len);
        } else if (it->type == JSON_OBJECT) {
            const json *dv = json_get(it, "d");
            if (it->len != 1 || !dv || !json_is_int(dv) || dv->number <= 0) {
                snprintf(err, errlen, "ops[%zu]: a delete must be {\"d\": positive integer}", i);
                goto bad;
            }
            ot_delete(op, (int64_t)dv->number);
        } else {
            snprintf(err, errlen, "ops[%zu]: each op must be a number (retain), a string (insert) or {\"d\": n} (delete)", i);
            goto bad;
        }
    }
    return 0;
bad:
    ot_free(op);
    return -1;
}

json *ot_to_json(const ot_op *op) {
    json *a = json_arr();
    for (size_t i = 0; i < op->len; i++) {
        const ot_comp *c = &op->c[i];
        if (c->kind == OT_RETAIN) json_push(a, json_num((double)c->n));
        else if (c->kind == OT_INSERT) json_push(a, json_strn(c->s, c->slen));
        else {
            json *d = json_obj();
            json_set(d, "d", json_num((double)c->n));
            json_push(a, d);
        }
    }
    return a;
}

int ot_is_noop(const ot_op *op) {
    for (size_t i = 0; i < op->len; i++)
        if (op->c[i].kind != OT_RETAIN) return 0;
    return 1;
}

int ot_transform(const ot_op *a, const ot_op *b, ot_op *a2, ot_op *b2) {
    ot_op ta, tb;
    ot_init(&ta);
    ot_init(&tb);
    if (a2) ot_init(a2);
    if (b2) ot_init(b2);
    if (a->base_len != b->base_len) return -1;
    size_t i = 0, j = 0;
    const ot_comp *ca = NULL, *cb = NULL;
    int64_t ra = 0, rb = 0; /* what is left of ca / cb */
    for (;;) {
        if (!ca && i < a->len) { ca = &a->c[i++]; ra = ca->n; }
        if (!cb && j < b->len) { cb = &b->c[j++]; rb = cb->n; }
        if (!ca && !cb) break;
        /* inserts go through untouched; on a tie a's insert comes first */
        if (ca && ca->kind == OT_INSERT) {
            ot_insert(&ta, ca->s, ca->slen);
            ot_retain(&tb, ca->n);
            ca = NULL;
            continue;
        }
        if (cb && cb->kind == OT_INSERT) {
            ot_retain(&ta, cb->n);
            ot_insert(&tb, cb->s, cb->slen);
            cb = NULL;
            continue;
        }
        if (!ca || !cb) { /* unreachable with equal base lengths */
            ot_free(&ta);
            ot_free(&tb);
            return -1;
        }
        int64_t m = ra < rb ? ra : rb;
        if (ca->kind == OT_RETAIN && cb->kind == OT_RETAIN) {
            ot_retain(&ta, m);
            ot_retain(&tb, m);
        } else if (ca->kind == OT_DELETE && cb->kind == OT_RETAIN) {
            ot_delete(&ta, m);
        } else if (ca->kind == OT_RETAIN && cb->kind == OT_DELETE) {
            ot_delete(&tb, m);
        } /* delete / delete: both removed the same text */
        ra -= m;
        rb -= m;
        if (!ra) ca = NULL;
        if (!rb) cb = NULL;
    }
    if (a2) *a2 = ta; else ot_free(&ta);
    if (b2) *b2 = tb; else ot_free(&tb);
    return 0;
}

/* Advance n UTF-16 units from byte *pos; copies the bytes when out is set. */
static int advance(const char *text, size_t len, size_t *pos, int64_t n, sbuf *out, char *err, size_t errlen) {
    const unsigned char *p = (const unsigned char *)text;
    size_t start = *pos, i = *pos;
    while (n > 0) {
        if (i >= len) { snprintf(err, errlen, "ops span more than the document length"); return -1; }
        unsigned cp;
        size_t k = dec(p + i, len - i, &cp);
        if (!k) { snprintf(err, errlen, "the document is not valid UTF-8"); return -1; }
        int64_t u = cp >= 0x10000 ? 2 : 1;
        if (u > n) { snprintf(err, errlen, "an op boundary splits a surrogate pair"); return -1; }
        i += k;
        n -= u;
    }
    if (out) sbuf_put(out, text + start, i - start);
    *pos = i;
    return 0;
}

int ot_apply(const char *text, size_t len, const ot_op *op, sbuf *out, char *err, size_t errlen) {
    size_t pos = 0;
    for (size_t i = 0; i < op->len; i++) {
        const ot_comp *c = &op->c[i];
        if (c->kind == OT_INSERT) {
            if (out) sbuf_put(out, c->s, c->slen);
        } else if (advance(text, len, &pos, c->n, c->kind == OT_RETAIN ? out : NULL, err, errlen) != 0) {
            return -1;
        }
    }
    if (pos != len) {
        snprintf(err, errlen, "ops must span the whole document (%lld units given)", (long long)op->base_len);
        return -1;
    }
    if (out && !out->data) sbuf_reserve(out, 0);
    return 0;
}
