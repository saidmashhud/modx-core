#include "modx/idem.h"
#include "modx/ws.h"

#include <stdlib.h>
#include <string.h>

int mx_idem_key_valid(const char *key) {
    if (!key) return 0;
    size_t n = strlen(key);
    if (n == 0 || n > MX_IDEM_KEY_MAX || key[0] == ' ' || key[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)key[i];
        if (ch < 0x20 || ch > 0x7e) return 0;
    }
    return 1;
}

int mx_idem_method(const char *m) {
    return m && (strcmp(m, "POST") == 0 || strcmp(m, "PUT") == 0 || strcmp(m, "PATCH") == 0 || strcmp(m, "DELETE") == 0);
}

int mx_idem_storable(const http_response *resp) {
    if (!resp || resp->sse || resp->ws || resp->file_fd >= 0) return 0;
    int s = resp->status;
    if (s == 408 || s == 429) return 0;
    return (s >= 200 && s < 300) || (s >= 400 && s < 500);
}

json *mx_idem_record(const char *id, const http_request *req, const http_response *resp, int64_t now,
                     size_t max_body) {
    json *o = json_obj();
    json_set(o, "id", json_str(id));
    json_set(o, "method", json_str(req->method));
    json_set(o, "target", json_str(req->target ? req->target : (req->path ? req->path : "")));
    json_set(o, "status", json_num(resp->status));
    json_set(o, "content_type", resp->content_type ? json_str(resp->content_type) : json_null());
    size_t n = resp->body.len;
    const char *b = resp->body.data ? resp->body.data : "";
    if (n <= max_body && memchr(b, 0, n) == NULL && mx_utf8_valid((const unsigned char *)b, n))
        json_set(o, "body", json_strn(b, n));
    else
        json_set(o, "body_omitted", json_bool(1));
    json_set(o, "created_at", json_num((double)now));
    return o;
}

int mx_idem_matches(const json *rec, const http_request *req) {
    const char *m = json_get_str(rec, "method"), *t = json_get_str(rec, "target");
    const char *want = req->target ? req->target : (req->path ? req->path : "");
    return m && t && strcmp(m, req->method) == 0 && strcmp(t, want) == 0;
}

int mx_idem_expired(const json *rec, int64_t now, int64_t ttl_ms) {
    const json *c = json_get(rec, "created_at");
    if (!c || c->type != JSON_NUMBER) return 1;
    return (int64_t)c->number + ttl_ms <= now;
}

static const char JSON_CT[] = "application/json; charset=utf-8";

void mx_idem_replay(const json *rec, http_response *resp) {
    resp->status = (int)json_get_num(rec, "status", 200);
    const char *ct = json_get_str(rec, "content_type");
    resp->content_type = !ct ? NULL : strcmp(ct, JSON_CT) == 0 ? JSON_CT : ct;
    resp->body.len = 0;
    const json *b = json_get(rec, "body");
    if (b && b->type == JSON_STRING) {
        sbuf_put(&resp->body, b->str, b->str_len);
    } else {
        resp->content_type = JSON_CT;
        sbuf_puts(&resp->body, "{\"replayed\":true,\"body_omitted\":true}");
    }
    sbuf_puts(&resp->headers, MX_IDEM_REPLAYED_HEADER);
}

long mx_idem_prune(mx_store *st, int coll, int64_t cutoff) {
    size_t n;
    const json **rows = mx_store_all(st, coll, &n);
    char **ids = malloc((n ? n : 1) * sizeof *ids);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const json *c = json_get(rows[i], "created_at");
        const char *id = json_get_str(rows[i], "id");
        if (id && (!c || c->type != JSON_NUMBER || (int64_t)c->number < cutoff)) ids[k++] = xstrdup(id);
    }
    free(rows);
    long rc = 0;
    for (size_t i = 0; i < k; i++) {
        if (rc >= 0 && mx_store_del(st, coll, ids[i], NULL) != 0) rc = -1;
        else if (rc >= 0) rc++;
        free(ids[i]);
    }
    free(ids);
    return rc;
}

static int cmp_created(const void *a, const void *b) {
    double x = json_get_num(*(const json *const *)a, "created_at", 0), y = json_get_num(*(const json *const *)b, "created_at", 0);
    if (x != y) return x < y ? -1 : 1;
    /* same millisecond: by id, so the order is stable */
    const char *p = json_get_str(*(const json *const *)a, "id"), *q = json_get_str(*(const json *const *)b, "id");
    return strcmp(p ? p : "", q ? q : "");
}

long mx_idem_cap(mx_store *st, int coll, const char *field, const char *value, size_t max) {
    if (!value) return 0;
    if (mx_store_find_count(st, coll, field, value) < max) return 0;
    size_t n;
    const json **rows = mx_store_find(st, coll, field, value, &n);
    if (n < max) { free(rows); return 0; }
    qsort(rows, n, sizeof *rows, cmp_created);
    size_t drop = n - (max ? max - 1 : 0);
    char **ids = malloc(drop * sizeof *ids);
    size_t k = 0;
    for (size_t i = 0; i < drop; i++) {
        const char *id = json_get_str(rows[i], "id");
        if (id) ids[k++] = xstrdup(id);
    }
    free(rows);
    long rc = 0;
    for (size_t i = 0; i < k; i++) {
        if (rc >= 0 && mx_store_del(st, coll, ids[i], NULL) != 0) rc = -1;
        else if (rc >= 0) rc++;
        free(ids[i]);
    }
    free(ids);
    return rc;
}
