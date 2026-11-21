/* Idempotency keys: key validation, which methods and answers count, records
 * (body, oversized / binary bodies), method + target matching, replay,
 * expiry, pruning by age and the per-owner cap, and records surviving a
 * reopen of the store. */
#include <stdlib.h>

#include "modx/idem.h"
#include "test.h"
#include "test_util.h"

enum { I_ROWS, I_KEYS, I_NCOLL };
static const char *const OWNER_IX[] = { "owner", NULL };
static const mx_coll_def IDEFS[I_NCOLL] = {
    { "rows", 0, NULL },
    { "idempotency_keys", MX_COLL_PRIVATE | MX_COLL_NO_RING, OWNER_IX },
};

static mx_store *open_idem(const char *dir) {
    char err[256];
    mx_store *st = mx_store_open(dir, IDEFS, I_NCOLL, 100, NULL, err, sizeof err);
    if (!st) fprintf(stderr, "  mx_store_open: %s\n", err);
    return st;
}

static void req_init(http_request *r, const char *method, const char *target) {
    memset(r, 0, sizeof *r);
    snprintf(r->method, sizeof r->method, "%s", method);
    r->target = (char *)target;
    r->path = (char *)target;
}

static void resp_init(http_response *r, int status, const char *body) {
    memset(r, 0, sizeof *r);
    r->status = status;
    r->file_fd = -1;
    r->content_type = "application/json; charset=utf-8";
    sbuf_init(&r->body);
    sbuf_init(&r->headers);
    if (body) sbuf_puts(&r->body, body);
}

static void resp_free(http_response *r) {
    sbuf_free(&r->body);
    sbuf_free(&r->headers);
}

static void t_keys(void) {
    CHECK(mx_idem_key_valid("a"));
    CHECK(mx_idem_key_valid("op_mhx1_2"));
    CHECK(mx_idem_key_valid("with inner space"));
    CHECK(!mx_idem_key_valid(""));
    CHECK(!mx_idem_key_valid(NULL));
    CHECK(!mx_idem_key_valid(" lead") && !mx_idem_key_valid("trail "));
    CHECK(!mx_idem_key_valid("tab\there") && !mx_idem_key_valid("nl\n") && !mx_idem_key_valid("del\x7f"));
    CHECK(!mx_idem_key_valid("\xd0\x9a")); /* not ASCII */
    char k[MX_IDEM_KEY_MAX + 2];
    memset(k, 'k', MX_IDEM_KEY_MAX);
    k[MX_IDEM_KEY_MAX] = 0;
    CHECK(mx_idem_key_valid(k));
    k[MX_IDEM_KEY_MAX] = 'k';
    k[MX_IDEM_KEY_MAX + 1] = 0;
    CHECK(!mx_idem_key_valid(k));

    CHECK(mx_idem_method("POST") && mx_idem_method("PUT") && mx_idem_method("PATCH") && mx_idem_method("DELETE"));
    CHECK(!mx_idem_method("GET") && !mx_idem_method("HEAD") && !mx_idem_method("OPTIONS") && !mx_idem_method("post"));

    http_response r;
    int ok[] = { 200, 201, 204, 400, 403, 404, 409, 422 }, no[] = { 100, 301, 408, 429, 500, 503 };
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) {
        resp_init(&r, ok[i], NULL);
        CHECK(mx_idem_storable(&r));
    }
    for (size_t i = 0; i < sizeof no / sizeof *no; i++) {
        resp_init(&r, no[i], NULL);
        CHECK(!mx_idem_storable(&r));
    }
    resp_init(&r, 200, NULL);
    r.sse = 1;
    CHECK(!mx_idem_storable(&r));
    resp_init(&r, 200, NULL);
    r.file_fd = 3;
    CHECK(!mx_idem_storable(&r));
}

static void t_record_replay(void) {
    http_request q;
    req_init(&q, "POST", "/api/items?x=1");
    http_response r;
    resp_init(&r, 201, "{\"id\":\"i1\",\"title\":\"Привет\"}");
    json *rec = mx_idem_record("acc|key", &q, &r, 1000, 1024);
    CHECK_STR(json_get_str(rec, "id"), "acc|key");
    CHECK_STR(json_get_str(rec, "method"), "POST");
    CHECK_STR(json_get_str(rec, "target"), "/api/items?x=1");
    CHECK(json_get_num(rec, "status", 0) == 201 && json_get_num(rec, "created_at", 0) == 1000);
    CHECK_STR(json_get_str(rec, "body"), "{\"id\":\"i1\",\"title\":\"Привет\"}");
    resp_free(&r);

    CHECK(mx_idem_matches(rec, &q));
    http_request q2;
    req_init(&q2, "PATCH", "/api/items?x=1");
    CHECK(!mx_idem_matches(rec, &q2));
    req_init(&q2, "POST", "/api/items?x=2");
    CHECK(!mx_idem_matches(rec, &q2));
    req_init(&q2, "POST", "/api/items");
    CHECK(!mx_idem_matches(rec, &q2));

    CHECK(!mx_idem_expired(rec, 1000, 500) && !mx_idem_expired(rec, 1499, 500) && mx_idem_expired(rec, 1500, 500));
    json *bare = json_obj();
    CHECK(mx_idem_expired(bare, 0, 1000));
    json_free(bare);

    resp_init(&r, 200, "something else");
    sbuf_puts(&r.headers, "X-Other: 1\r\n");
    mx_idem_replay(rec, &r);
    CHECK(r.status == 201 && strcmp(r.content_type, "application/json; charset=utf-8") == 0);
    CHECK(r.body.len == strlen("{\"id\":\"i1\",\"title\":\"Привет\"}") && memcmp(r.body.data, "{\"id\":\"i1\"", 10) == 0);
    CHECK(r.headers.data && strstr(r.headers.data, "Idempotent-Replayed: true\r\n"));
    resp_free(&r);
    json_free(rec);

    /* a custom content type is replayed as stored; an empty 204 body too */
    resp_init(&r, 204, NULL);
    r.content_type = "text/plain";
    rec = mx_idem_record("k2", &q, &r, 1, 1024);
    resp_free(&r);
    resp_init(&r, 200, "x");
    mx_idem_replay(rec, &r);
    CHECK(r.status == 204 && r.body.len == 0 && strcmp(r.content_type, "text/plain") == 0);
    resp_free(&r);
    json_free(rec);

    /* too large, or not text: a marker instead of the body */
    char big[2048];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    resp_init(&r, 201, big);
    rec = mx_idem_record("k3", &q, &r, 1, 1024);
    CHECK(!json_get(rec, "body") && json_get(rec, "body_omitted") && json_get(rec, "body_omitted")->boolean);
    resp_free(&r);
    resp_init(&r, 200, NULL);
    mx_idem_replay(rec, &r);
    CHECK(r.status == 201 && r.body.len && strstr(r.body.data, "\"body_omitted\":true"));
    resp_free(&r);
    json_free(rec);
    resp_init(&r, 201, NULL);
    sbuf_put(&r.body, "a\0b", 3);
    rec = mx_idem_record("k4", &q, &r, 1, 1024);
    CHECK(json_get(rec, "body_omitted") && !json_get(rec, "body"));
    resp_free(&r);
    json_free(rec);
    resp_init(&r, 201, "\xff\xfe");
    rec = mx_idem_record("k5", &q, &r, 1, 1024);
    CHECK(json_get(rec, "body_omitted"));
    resp_free(&r);
    json_free(rec);
}

static json *rec_for(const char *id, const char *owner, int64_t at) {
    http_request q;
    req_init(&q, "POST", "/x");
    http_response r;
    resp_init(&r, 201, "{}");
    json *rec = mx_idem_record(id, &q, &r, at, 1024);
    json_set(rec, "owner", json_str(owner));
    resp_free(&r);
    return rec;
}

static void t_prune_cap_persist(void) {
    char *dir = make_tmpdir();
    mx_store *st = open_idem(dir);
    CHECK(st != NULL);
    if (!st) { rm_rf(dir); return; }
    char id[32];
    for (int i = 0; i < 10; i++) {
        snprintf(id, sizeof id, "a|%d", i);
        CHECK(mx_store_put(st, I_KEYS, rec_for(id, "a", 100 + i), NULL) == 0);
    }
    for (int i = 0; i < 3; i++) {
        snprintf(id, sizeof id, "b|%d", i);
        CHECK(mx_store_put(st, I_KEYS, rec_for(id, "b", 100 + i), NULL) == 0);
    }
    CHECK(mx_store_sync(st) == 0);

    /* cap 5 for owner a: room for one more -> 4 newest stay */
    CHECK(mx_idem_cap(st, I_KEYS, "owner", "a", 5) == 6);
    CHECK(mx_store_find_count(st, I_KEYS, "owner", "a") == 4);
    CHECK(!mx_store_get(st, I_KEYS, "a|5") && mx_store_get(st, I_KEYS, "a|6") && mx_store_get(st, I_KEYS, "a|9"));
    CHECK(mx_store_find_count(st, I_KEYS, "owner", "b") == 3); /* other owners untouched */
    CHECK(mx_idem_cap(st, I_KEYS, "owner", "b", 5) == 0);
    CHECK(mx_idem_cap(st, I_KEYS, "owner", NULL, 5) == 0);

    /* age: everything created before 102 goes (b|0, b|1) */
    CHECK(mx_idem_prune(st, I_KEYS, 102) == 2);
    CHECK(mx_store_count(st, I_KEYS) == 5 && mx_store_get(st, I_KEYS, "b|2"));
    CHECK(mx_store_sync(st) == 0);
    mx_store_close(st);

    /* the records and the deletes are journaled: same state after a reopen */
    st = open_idem(dir);
    CHECK(st && mx_store_count(st, I_KEYS) == 5 && mx_store_get(st, I_KEYS, "a|9") && !mx_store_get(st, I_KEYS, "a|0"));
    CHECK(st && st->ring_len == 0); /* private, no ring slot */
    CHECK(st && mx_store_compact(st) == 0);
    mx_store_close(st);
    st = open_idem(dir);
    CHECK(st && mx_store_count(st, I_KEYS) == 5);
    CHECK(st && mx_idem_prune(st, I_KEYS, INT64_MAX) == 5 && mx_store_count(st, I_KEYS) == 0);
    mx_store_close(st);
    rm_rf(dir);
}

void test_idem_all(void) {
    RUN(t_keys);
    RUN(t_record_replay);
    RUN(t_prune_cap_persist);
}
