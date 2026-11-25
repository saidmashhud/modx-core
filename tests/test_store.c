/* Store: hash map, journal append + reload, torn tails and CRC mismatches,
 * compaction, corrupt snapshots, the change ring and its window, listener
 * only after fsync, collection flags (private / no ring / redact), the
 * row_key / journal_extra / scope / on_apply hooks. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "modx/store.h"
#include "test.h"
#include "test_util.h"

enum { T_ITEMS, T_LABELS, T_SECRETS, T_USERS, T_BULK, T_NCOLL };
static const char *const ITEM_INDEX[] = { "tag", NULL };
static const mx_coll_def DEFS[T_NCOLL] = {
    { "items", 0, ITEM_INDEX },
    { "labels", 0, NULL },
    { "secrets", MX_COLL_PRIVATE, NULL },
    { "users", MX_COLL_REDACT, NULL },
    { "bulk", MX_COLL_PRIVATE | MX_COLL_NO_RING, NULL },
};

/* users rows live per group: key "<group>|<id>", the journal remembers the group */
static char *users_key(void *ctx, int coll, const char *id, const json *rec, const json *row) {
    (void)ctx;
    if (coll != T_USERS) return NULL;
    const char *g = rec ? json_get_str(rec, "group") : NULL;
    if (!g) g = json_get_str(row, "group");
    sbuf k;
    sbuf_init(&k);
    sbuf_puts(&k, g ? g : "");
    sbuf_putc(&k, '|');
    sbuf_puts(&k, id);
    return sbuf_take(&k);
}

static void users_extra(void *ctx, int coll, const char *id, const char *key, const json *row, sbuf *out) {
    (void)ctx; (void)id; (void)row;
    if (coll != T_USERS) return;
    const char *bar = strchr(key, '|');
    sbuf_puts(out, ",\"group\":");
    json_write_string(out, key, bar ? (size_t)(bar - key) : 0);
}

static void users_public(void *ctx, int coll, const json *row, sbuf *out) {
    (void)ctx; (void)coll;
    json *c = json_clone(row);
    json_del(c, "password_hash");
    json_write(out, c);
    json_free(c);
}

static int scope_calls, apply_calls, apply_with_old, snap_calls;
static void *scope_new(void *ctx, mx_store *st, const mx_commit_info *ci) {
    (void)ctx; (void)st;
    scope_calls++;
    const json *r = ci->row ? ci->row : ci->old;
    const char *owner = json_get_str(r, "owner");
    return owner ? xstrdup(owner) : NULL;
}
static void scope_free(void *ctx, void *s) { (void)ctx; free(s); }
static void on_apply(void *ctx, mx_store *st, int coll, const char *key, const json *old, const json *row) {
    (void)ctx; (void)st; (void)coll; (void)key; (void)row;
    apply_calls++;
    if (old) apply_with_old++;
}
static void before_snap(void *ctx, mx_store *st) { (void)ctx; (void)st; snap_calls++; }

static const mx_store_hooks HOOKS = { NULL, users_key, users_extra, users_public, scope_new, scope_free, on_apply,
                                      before_snap };

static mx_store *open_st(const char *dir, size_t ring) {
    char err[256];
    mx_store *st = mx_store_open(dir, DEFS, T_NCOLL, ring, &HOOKS, err, sizeof err);
    if (!st) fprintf(stderr, "  mx_store_open: %s\n", err);
    return st;
}

static json *item(const char *id, int n) {
    json *o = json_obj();
    json_set(o, "id", json_str(id));
    json_set(o, "n", json_num(n));
    json_set(o, "title", json_str("Привет \"quoted\"\nline"));
    return o;
}

static void append_change(void *ctx, const mx_change *ch) {
    sbuf *out = ctx;
    if (out->len > 1) sbuf_putc(out, ',');
    sbuf_puts(out, ch->json);
}

static int changes_since(mx_store *st, int64_t since, sbuf *out) {
    out->len = 0;
    sbuf_putc(out, '[');
    int rc = mx_store_for_each_since(st, since, NULL, NULL, append_change, out);
    sbuf_putc(out, ']');
    return rc;
}

static void t_hmap(void) {
    hmap m;
    hmap_init(&m);
    char key[32];
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof key, "k%d", i);
        hmap_put(&m, key, json_num(i));
    }
    CHECK(m.count == 2000);
    for (int i = 0; i < 2000; i += 2) {
        snprintf(key, sizeof key, "k%d", i);
        CHECK(hmap_del(&m, key) == 1);
    }
    CHECK(m.count == 1000);
    int ok = 1;
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof key, "k%d", i);
        json *v = hmap_get(&m, key);
        if (i % 2 == 0 ? v != NULL : (!v || v->number != i)) ok = 0;
    }
    CHECK(ok);
    CHECK(hmap_del(&m, "missing") == 0);
    json *old = hmap_swap(&m, "k1", json_num(-1));
    CHECK(old && old->number == 1);
    json_free(old);
    CHECK(hmap_get(&m, "k1")->number == -1);
    json *t = hmap_take(&m, "k3");
    CHECK(t && t->number == 3 && !hmap_get(&m, "k3") && m.count == 999);
    json_free(t);
    hmap_free(&m);
}

static void t_append_reload(void) {
    char *dir = make_tmpdir();
    mx_store *st = open_st(dir, 100);
    CHECK(st && mx_store_is_empty(st));
    CHECK(mx_store_coll(st, "labels") == T_LABELS && mx_store_coll(st, "nope") == -1);
    CHECK(mx_store_put(st, T_ITEMS, item("a", 1), NULL) == 0);
    CHECK(mx_store_put(st, T_ITEMS, item("b", 2), "client-1") == 0);
    CHECK(mx_store_put(st, T_ITEMS, item("a", 10), NULL) == 0); /* overwrite */
    CHECK(mx_store_del(st, T_ITEMS, "b", NULL) == 0);
    CHECK(mx_store_dirty(st));
    CHECK(mx_store_sync(st) == 0);
    CHECK(!mx_store_dirty(st));
    CHECK(st->seq == 4);
    CHECK(mx_store_put(st, 99, item("x", 1), NULL) == -1); /* unknown collection */
    CHECK(mx_store_put(st, T_ITEMS, json_obj(), NULL) == -1); /* no id */
    mx_store_close(st);

    st = open_st(dir, 100);
    CHECK(st && st->seq == 4);
    CHECK(mx_store_count(st, T_ITEMS) == 1);
    CHECK(mx_store_get(st, T_ITEMS, "b") == NULL);
    CHECK(json_get_num(mx_store_get(st, T_ITEMS, "a"), "n", 0) == 10);
    CHECK_STR(json_get_str(mx_store_get(st, T_ITEMS, "a"), "title"), "Привет \"quoted\"\nline");
    sbuf out;
    sbuf_init(&out);
    CHECK(changes_since(st, 1, &out) == 0);
    json *arr = json_parse(out.data, out.len, NULL, 0);
    CHECK(arr && arr->len == 3);
    if (arr && arr->len == 3) {
        CHECK_STR(json_get_str(arr->items[0], "client_id"), "client-1");
        CHECK_STR(json_get_str(arr->items[2], "op"), "del");
        CHECK(json_get(arr->items[2], "data")->type == JSON_NULL);
    }
    json_free(arr);
    sbuf_free(&out);
    mx_store_close(st);
    rm_rf(dir);
}

static void t_torn_tail(void) {
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log");
    mx_store *st = open_st(dir, 100);
    mx_store_put(st, T_ITEMS, item("a", 1), NULL);
    mx_store_put(st, T_ITEMS, item("b", 2), NULL);
    mx_store_sync(st);
    mx_store_close(st);
    long good = file_size(jp);
    append_raw(jp, "0badc0de {\"seq\":3,\"entity\":\"items\",\"op\":\"put\",\"id\":\"c\",\"da");
    st = open_st(dir, 100);
    CHECK(st && st->recovered_truncated == 1 && st->seq == 2);
    CHECK(mx_store_get(st, T_ITEMS, "c") == NULL);
    CHECK(file_size(jp) == good);
    CHECK(mx_store_put(st, T_ITEMS, item("c", 3), NULL) == 0 && mx_store_sync(st) == 0);
    mx_store_close(st);
    st = open_st(dir, 100);
    CHECK(st && st->recovered_truncated == 0 && st->seq == 3 && mx_store_count(st, T_ITEMS) == 3);
    mx_store_close(st);
    /* a record cut right before its newline is torn too */
    char *line = NULL;
    {
        sbuf b;
        sbuf_init(&b);
        sbuf_puts(&b, "{\"seq\":4,\"entity\":\"items\",\"op\":\"put\",\"id\":\"d\",\"data\":{\"id\":\"d\"},\"ts\":1}");
        char hdr[16];
        snprintf(hdr, sizeof hdr, "%08x ", crc32_buf(b.data, b.len));
        sbuf rec;
        sbuf_init(&rec);
        sbuf_puts(&rec, hdr);
        sbuf_put(&rec, b.data, b.len);
        line = sbuf_take(&rec);
        sbuf_free(&b);
    }
    append_raw(jp, line); /* complete record, but no '\n' */
    st = open_st(dir, 100);
    CHECK(st && st->recovered_truncated == 1 && st->seq == 3 && !mx_store_get(st, T_ITEMS, "d"));
    mx_store_close(st);
    free(line);
    free(jp);
    rm_rf(dir);
}

/* A well-formed record of a collection this build does not know (a newer
 * version wrote it) is skipped, its seq kept; the records after it stay. */
static void t_unknown_collection(void) {
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log");
    mx_store *st = open_st(dir, 100);
    mx_store_put(st, T_ITEMS, item("a", 1), NULL);
    mx_store_sync(st);
    mx_store_close(st);
    const char *recs[] = {
        "{\"seq\":2,\"entity\":\"from_the_future\",\"op\":\"put\",\"id\":\"f\",\"data\":{\"id\":\"f\"},\"ts\":1}",
        "{\"seq\":3,\"entity\":\"items\",\"op\":\"put\",\"id\":\"b\",\"data\":{\"id\":\"b\"},\"ts\":1}",
    };
    for (int i = 0; i < 2; i++) {
        char hdr[16];
        snprintf(hdr, sizeof hdr, "%08x ", crc32_buf(recs[i], strlen(recs[i])));
        sbuf rec;
        sbuf_init(&rec);
        sbuf_puts(&rec, hdr);
        sbuf_puts(&rec, recs[i]);
        sbuf_putc(&rec, '\n');
        append_raw(jp, rec.data);
        sbuf_free(&rec);
    }
    long size = file_size(jp);
    st = open_st(dir, 100);
    CHECK(st && st->recovered_truncated == 0 && st->skipped_records == 1 && st->seq == 3);
    CHECK(st && mx_store_get(st, T_ITEMS, "b") && file_size(jp) == size);
    /* the next seq does not reuse the skipped one */
    CHECK(st && mx_store_put(st, T_ITEMS, item("c", 3), NULL) == 0 && mx_store_sync(st) == 0 && st->seq == 4);
    mx_store_close(st);
    st = open_st(dir, 100);
    CHECK(st && st->seq == 4 && mx_store_count(st, T_ITEMS) == 3);
    mx_store_close(st);
    free(jp);
    rm_rf(dir);
}

static void t_crc_mismatch(void) {
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log");
    mx_store *st = open_st(dir, 100);
    mx_store_put(st, T_ITEMS, item("a", 1), NULL);
    mx_store_sync(st);
    mx_store_close(st);
    long good = file_size(jp);
    /* bad CRC, then a record that would be valid: everything from the bad one on is dropped */
    append_raw(jp, "00000000 {\"seq\":2,\"entity\":\"items\",\"op\":\"put\",\"id\":\"x\",\"data\":{\"id\":\"x\"},\"ts\":1}\n");
    st = open_st(dir, 100);
    CHECK(st && st->seq == 1 && !mx_store_get(st, T_ITEMS, "x") && file_size(jp) == good);
    CHECK(mx_store_put(st, T_ITEMS, item("x", 2), NULL) == 0 && mx_store_sync(st) == 0);
    mx_store_close(st);
    st = open_st(dir, 100);
    CHECK(st && st->seq == 2 && mx_store_get(st, T_ITEMS, "x"));
    mx_store_close(st);
    /* garbage that is not even hex */
    append_raw(jp, "zzzzzzzz {}\n");
    st = open_st(dir, 100);
    CHECK(st && st->recovered_truncated && st->seq == 2);
    mx_store_close(st);
    free(jp);
    rm_rf(dir);
}

static void t_compaction(void) {
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log"), *sp = file_path(dir, "snapshot.json");
    mx_store *st = open_st(dir, 1000);
    char id[16];
    for (int i = 0; i < 50; i++) {
        snprintf(id, sizeof id, "i%d", i);
        mx_store_put(st, T_ITEMS, item(id, i), NULL);
    }
    mx_store_del(st, T_ITEMS, "i7", NULL);
    mx_store_sync(st);
    snap_calls = 0;
    CHECK(mx_store_compact(st) == 0);
    CHECK(snap_calls == 1);
    CHECK(file_size(jp) == 0 && file_size(sp) > 0);
    mx_store_put(st, T_LABELS, item("after", 1), NULL);
    mx_store_sync(st);
    mx_store_close(st);
    st = open_st(dir, 1000);
    CHECK(st && st->seq == 52);
    CHECK(mx_store_count(st, T_ITEMS) == 49 && !mx_store_get(st, T_ITEMS, "i7"));
    CHECK(json_get_num(mx_store_get(st, T_ITEMS, "i42"), "n", 0) == 42);
    CHECK(mx_store_get(st, T_LABELS, "after") != NULL);
    sbuf out;
    sbuf_init(&out);
    CHECK(changes_since(st, 51, &out) == 0);  /* the journal record after the snapshot */
    CHECK(changes_since(st, 10, &out) == -1); /* compacted away */
    sbuf_free(&out);
    /* automatic compaction past the threshold */
    st->compact_threshold = 2048;
    for (int i = 50; i < 70; i++) {
        snprintf(id, sizeof id, "i%d", i);
        mx_store_put(st, T_ITEMS, item(id, i), NULL);
        mx_store_sync(st);
    }
    mx_store_tick(st);
    CHECK(file_size(jp) < 2048);
    mx_store_close(st);
    st = open_st(dir, 1000);
    CHECK(st && mx_store_count(st, T_ITEMS) == 69 && st->seq == 72);
    mx_store_close(st);
    /* corrupt snapshot is fatal (never silently start empty) */
    int fd = open(sp, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0 && write(fd, "{\"seq\":1,", 9) == 9);
    close(fd);
    char err[256];
    st = mx_store_open(dir, DEFS, T_NCOLL, 10, &HOOKS, err, sizeof err);
    CHECK(st == NULL && strstr(err, "corrupt snapshot") != NULL);
    free(jp);
    free(sp);
    rm_rf(dir);
}

static int listener_calls;
static int64_t listener_last;
static char listener_json[512];
static void count_listener(void *ctx, const mx_change *e) {
    (void)ctx;
    listener_calls++;
    listener_last = e->seq;
    snprintf(listener_json, sizeof listener_json, "%s", e->json);
}

static void t_ring_window(void) {
    char *dir = make_tmpdir();
    mx_store *st = open_st(dir, 5);
    st->listener = count_listener;
    listener_calls = 0;
    char id[16];
    for (int i = 0; i < 10; i++) {
        snprintf(id, sizeof id, "r%d", i);
        mx_store_put(st, T_ITEMS, item(id, i), NULL);
    }
    CHECK(listener_calls == 0); /* nothing is handed out before fsync */
    sbuf out;
    sbuf_init(&out);
    CHECK(changes_since(st, 5, &out) == 0 && strcmp(out.data, "[]") == 0); /* nor replayed */
    CHECK(mx_store_sync(st) == 0);
    CHECK(listener_calls == 5 && listener_last == 10); /* the ring holds the last 5 */
    CHECK(changes_since(st, 4, &out) == -1);
    CHECK(changes_since(st, 5, &out) == 0);
    json *arr = json_parse(out.data, out.len, NULL, 0);
    CHECK(arr && arr->len == 5 && json_get_num(arr->items[0], "seq", 0) == 6);
    json_free(arr);
    CHECK(changes_since(st, 10, &out) == 0 && strcmp(out.data, "[]") == 0);
    CHECK(changes_since(st, 11, &out) == -1); /* ahead of the server */
    CHECK(mx_store_ring_at(st, 0)->seq == 6);
    sbuf_free(&out);
    mx_store_close(st);
    rm_rf(dir);
}

static void t_flags_hooks(void) {
    char *dir = make_tmpdir();
    mx_store *st = open_st(dir, 100);
    st->listener = count_listener;
    listener_calls = 0;
    scope_calls = apply_calls = apply_with_old = 0;
    json *u = item("alice", 1);
    json_set(u, "group", json_str("g1"));
    json_set(u, "password_hash", json_str("SECRETHASH"));
    json_set(u, "owner", json_str("alice"));
    CHECK(mx_store_put(st, T_USERS, u, NULL) == 0);
    CHECK(mx_store_put(st, T_SECRETS, item("s1", 1), NULL) == 0);
    CHECK(mx_store_put(st, T_BULK, item("b1", 1), NULL) == 0);
    CHECK(mx_store_sync(st) == 0);
    /* compound key; the row keeps its own id */
    CHECK(mx_store_get(st, T_USERS, "g1|alice") && !mx_store_get(st, T_USERS, "alice"));
    /* only the users change is public, redacted and without the journal-only field */
    CHECK(listener_calls == 1);
    CHECK(strstr(listener_json, "\"alice\"") && !strstr(listener_json, "SECRETHASH") &&
          !strstr(listener_json, "\"group\":\"g1\",\"ts\""));
    CHECK(st->ring_len == 2); /* users + secrets (NULL json); bulk takes no slot */
    CHECK(mx_store_ring_at(st, 0)->scope && strcmp(mx_store_ring_at(st, 0)->scope, "alice") == 0);
    CHECK(mx_store_ring_at(st, 1)->json == NULL);
    CHECK(scope_calls == 2 && apply_calls == 3 && apply_with_old == 0);
    json *u2 = json_clone(mx_store_get(st, T_USERS, "g1|alice"));
    json_set(u2, "n", json_num(2));
    CHECK(mx_store_put(st, T_USERS, u2, NULL) == 0 && apply_with_old == 1);
    CHECK(mx_store_del_key(st, T_USERS, "alice", "g1|alice", NULL) == 0 && apply_with_old == 2);
    CHECK(mx_store_sync(st) == 0 && listener_calls == 3);
    CHECK(strstr(listener_json, "\"op\":\"del\",\"id\":\"alice\""));
    json *u3 = item("bob", 1);
    json_set(u3, "group", json_str("g2"));
    json_set(u3, "password_hash", json_str("H2"));
    mx_store_put(st, T_USERS, u3, NULL);
    mx_store_sync(st);
    mx_store_close(st);
    /* replay: the journal's group field rebuilds the key, secrets survive on disk */
    scope_calls = 0;
    st = open_st(dir, 100);
    CHECK(st && mx_store_get(st, T_USERS, "g2|bob") && !mx_store_get(st, T_USERS, "g1|alice"));
    CHECK_STR(json_get_str(mx_store_get(st, T_USERS, "g2|bob"), "password_hash"), "H2");
    CHECK(mx_store_get(st, T_SECRETS, "s1") && mx_store_get(st, T_BULK, "b1"));
    CHECK(scope_calls == 5); /* every ring-taking record of the journal */
    sbuf out;
    sbuf_init(&out);
    CHECK(changes_since(st, 0, &out) == 0 && !strstr(out.data, "SECRETHASH") && !strstr(out.data, "H2"));
    sbuf_free(&out);
    /* snapshot load computes keys from rows */
    CHECK(mx_store_compact(st) == 0);
    mx_store_close(st);
    apply_calls = 0;
    st = open_st(dir, 100);
    CHECK(st && mx_store_get(st, T_USERS, "g2|bob") && apply_calls == 3);
    mx_store_close(st);
    rm_rf(dir);
}

static void fill(mx_store *st, int from, int to) {
    char id[16];
    for (int i = from; i < to; i++) {
        snprintf(id, sizeof id, "c%05d", i);
        mx_store_put(st, T_ITEMS, item(id, i), NULL);
    }
    mx_store_sync(st);
}

static void wait_bg(mx_store *st) {
    for (int i = 0; i < 2000 && st->compact_pid > 0; i++) { mx_store_tick(st); usleep(5000); }
}

static void t_background_compaction(void) {
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log"), *op = file_path(dir, "journal.old"), *sp = file_path(dir, "snapshot.json");
    mx_store *st = open_st(dir, 100);
    fill(st, 0, 3000);
    CHECK(mx_store_compact_bg(st) == 0 && st->compact_pid > 0);
    CHECK(mx_store_compact_bg(st) == 1);      /* one at a time */
    CHECK(file_size(jp) == 0 && file_size(op) > 0);
    fill(st, 3000, 3100);                      /* the journal keeps going meanwhile */
    mx_store_del(st, T_ITEMS, "c00007", NULL);
    mx_store_sync(st);
    wait_bg(st);
    CHECK(st->compact_pid == 0 && st->compactions == 1 && st->compact_failures == 0);
    CHECK(file_size(op) == -1 && file_size(sp) > 0 && file_size(jp) > 0);
    mx_store_close(st);
    st = open_st(dir, 100);
    CHECK(st && mx_store_count(st, T_ITEMS) == 3099 && !mx_store_get(st, T_ITEMS, "c00007"));
    CHECK(st && st->seq == 3101 && st->recovered_truncated == 0);
    mx_store_close(st);

    /* the child dies halfway through writing the snapshot */
    st = open_st(dir, 100);
    long snap_before = file_size(sp);
    fill(st, 4000, 4500);
    st->test_crash_compaction = 1;
    CHECK(mx_store_compact_bg(st) == 0);
    fill(st, 4500, 4600);
    wait_bg(st);
    CHECK(st->compact_failures == 1 && file_size(op) > 0); /* rotated journal kept */
    CHECK(file_size(sp) == snap_before);                    /* old snapshot untouched */
    mx_store_close(st);
    st = open_st(dir, 100);
    CHECK(st && mx_store_count(st, T_ITEMS) == 3099 + 600 && st->seq == 3101 + 600);
    CHECK(file_size(op) == -1); /* merged on open */
    /* after a failure the next compaction is a synchronous one that covers journal.old */
    mx_store_close(st);

    /* the parent dies after the child finished, before it dropped journal.old */
    st = open_st(dir, 100);
    fill(st, 5000, 5200);
    CHECK(mx_store_compact_bg(st) == 0);
    fill(st, 5200, 5250);
    int status;
    waitpid(st->compact_pid, &status, 0);      /* child done, parent never reaps: "crash" */
    st->compact_pid = 0;
    CHECK(file_size(op) > 0);
    mx_store *st2 = open_st(dir, 100);
    CHECK(st2 && mx_store_count(st2, T_ITEMS) == 3699 + 250 && st2->seq == 3701 + 250);
    CHECK(file_size(op) == -1);
    mx_store_close(st2);
    close(st->journal_fd); /* the "crashed" one writes nothing more */
    st->journal_fd = -1;
    mx_store_close(st);

    /* threshold-triggered compaction runs in the background */
    st = open_st(dir, 100);
    st->compact_threshold = 4096;
    fill(st, 6000, 6100);
    CHECK(st->compact_pid > 0 || st->compactions > 0);
    wait_bg(st);
    CHECK(st->compactions >= 1 && file_size(op) == -1);
    mx_store_close(st);
    st = open_st(dir, 100);
    CHECK(st && mx_store_count(st, T_ITEMS) == 3949 + 100);
    mx_store_close(st);
    free(jp);
    free(op);
    free(sp);
    rm_rf(dir);
}

static json *tagged(const char *id, const char *tag) {
    json *o = item(id, 1);
    if (tag) json_set(o, "tag", json_str(tag));
    return o;
}

static int has_id(const json **rows, size_t n, const char *id) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(json_get_str(rows[i], "id"), id) == 0) return 1;
    return 0;
}

static void t_indexes(void) {
    char *dir = make_tmpdir();
    mx_store *st = open_st(dir, 100);
    CHECK(mx_store_indexed(st, T_ITEMS, "tag") && !mx_store_indexed(st, T_ITEMS, "n"));
    mx_store_put(st, T_ITEMS, tagged("a", "red"), NULL);
    mx_store_put(st, T_ITEMS, tagged("b", "red"), NULL);
    mx_store_put(st, T_ITEMS, tagged("c", "blue"), NULL);
    mx_store_put(st, T_ITEMS, tagged("d", NULL), NULL);
    size_t n;
    const json **r = mx_store_find(st, T_ITEMS, "tag", "red", &n);
    CHECK(n == 2 && has_id(r, n, "a") && has_id(r, n, "b"));
    free(r);
    CHECK(mx_store_find_count(st, T_ITEMS, "tag", "blue") == 1);
    CHECK(mx_store_find_count(st, T_ITEMS, "tag", "green") == 0);
    /* value change moves the key, delete removes it, same value is a no-op */
    mx_store_put(st, T_ITEMS, tagged("a", "blue"), NULL);
    mx_store_put(st, T_ITEMS, tagged("c", "blue"), NULL);
    mx_store_del(st, T_ITEMS, "b", NULL);
    CHECK(mx_store_find_count(st, T_ITEMS, "tag", "red") == 0);
    r = mx_store_find(st, T_ITEMS, "tag", "blue", &n);
    CHECK(n == 2 && has_id(r, n, "a") && has_id(r, n, "c"));
    free(r);
    /* a row losing the field leaves the index */
    mx_store_put(st, T_ITEMS, tagged("c", NULL), NULL);
    CHECK(mx_store_find_count(st, T_ITEMS, "tag", "blue") == 1);
    /* unindexed fields scan */
    r = mx_store_find(st, T_LABELS, "tag", "blue", &n);
    CHECK(n == 0);
    free(r);
    mx_store_put(st, T_LABELS, tagged("l1", "blue"), NULL);
    CHECK(mx_store_find_count(st, T_LABELS, "tag", "blue") == 1);
    /* many rows in one bucket, then most removed */
    char id[16];
    for (int i = 0; i < 1000; i++) { snprintf(id, sizeof id, "m%d", i); mx_store_put(st, T_ITEMS, tagged(id, "many"), NULL); }
    for (int i = 0; i < 1000; i += 3) { snprintf(id, sizeof id, "m%d", i); mx_store_del(st, T_ITEMS, id, NULL); }
    CHECK(mx_store_find_count(st, T_ITEMS, "tag", "many") == 666);
    r = mx_store_find(st, T_ITEMS, "tag", "many", &n);
    CHECK(n == 666 && has_id(r, n, "m1") && !has_id(r, n, "m3"));
    free(r);
    mx_store_sync(st);
    mx_store_close(st);
    /* rebuilt from the journal, then from a snapshot */
    st = open_st(dir, 100);
    CHECK(mx_store_find_count(st, T_ITEMS, "tag", "many") == 666 && mx_store_find_count(st, T_ITEMS, "tag", "blue") == 1);
    CHECK(mx_store_compact(st) == 0);
    mx_store_close(st);
    st = open_st(dir, 100);
    CHECK(mx_store_find_count(st, T_ITEMS, "tag", "many") == 666 && mx_store_find_count(st, T_ITEMS, "tag", "red") == 0);
    mx_store_close(st);
    rm_rf(dir);
}

static void flip_bit(const char *path, long off, int mask) {
    FILE *f = fopen(path, "r+b");
    if (!f) { perror("flip"); exit(2); }
    fseek(f, off, SEEK_SET);
    int ch = fgetc(f);
    fseek(f, off, SEEK_SET);
    fputc(ch ^ mask, f);
    fclose(f);
}

/* Copy src to dst truncated to n bytes. */
static void copy_prefix(const char *src, const char *dst, long n) {
    FILE *in = fopen(src, "rb"), *out = fopen(dst, "wb");
    if (!in || !out) { perror("copy"); exit(2); }
    for (long i = 0; i < n; i++) {
        int ch = fgetc(in);
        if (ch == EOF) break;
        fputc(ch, out);
    }
    fclose(in);
    fclose(out);
}

static void t_crash_points(void) {
    /* a journal cut at every byte offset recovers exactly the complete records before the cut */
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log");
    mx_store *st = open_st(dir, 100);
    long ends[6] = { 0 };
    for (int i = 0; i < 5; i++) {
        char id[8];
        snprintf(id, sizeof id, "k%d", i);
        mx_store_put(st, T_ITEMS, item(id, i), NULL);
        mx_store_sync(st);
        ends[i + 1] = file_size(jp);
    }
    mx_store_close(st);
    char *full = file_path(dir, "journal.full");
    copy_prefix(jp, full, ends[5]);
    int ok = 1;
    for (long cut = 0; cut <= ends[5]; cut++) {
        copy_prefix(full, jp, cut);
        st = open_st(dir, 100);
        int complete = 0;
        while (complete < 5 && ends[complete + 1] <= cut) complete++;
        if (!st || st->seq != complete || (long)mx_store_count(st, T_ITEMS) != complete) ok = 0;
        if (st && file_size(jp) != ends[complete]) ok = 0; /* the torn tail is cut back */
        mx_store_close(st);
    }
    CHECK(ok);
    /* a flipped bit in a middle record is not a torn tail: open refuses (it used to drop records 3..5) */
    copy_prefix(full, jp, ends[5]);
    flip_bit(jp, ends[2] + 20, 0x20);
    {
        char err[512];
        long before = file_size(jp);
        mx_store *bad = mx_store_open(dir, DEFS, T_NCOLL, 100, &HOOKS, err, sizeof err);
        CHECK(bad == NULL && strstr(err, "corrupt record") && strstr(err, "salvage"));
        CHECK(file_size(jp) == before); /* nothing cut */
        mx_store_close(bad);
    }
    /* the last record flipped: nothing valid after it, so it is the (torn) tail */
    copy_prefix(full, jp, ends[5]);
    flip_bit(jp, ends[4] + 20, 0x20);
    st = open_st(dir, 100);
    CHECK(st && st->seq == 4 && st->recovered_truncated && file_size(jp) == ends[4]);
    mx_store_close(st);
    /* snapshots: a leftover tmp file is ignored, a truncated snapshot is refused */
    copy_prefix(full, jp, ends[5]);
    st = open_st(dir, 100);
    CHECK(mx_store_compact(st) == 0);
    mx_store_close(st);
    char *sp = file_path(dir, "snapshot.json"), *tp = file_path(dir, "snapshot.json.tmp");
    append_raw(tp, "{\"seq\":999,\"collections\":{\"items\":[{\"id\":\"ghost\"}]}}");
    st = open_st(dir, 100);
    CHECK(st && st->seq == 5 && !mx_store_get(st, T_ITEMS, "ghost"));
    mx_store_close(st);
    long ss = file_size(sp);
    char *sfull = file_path(dir, "snapshot.full");
    copy_prefix(sp, sfull, ss);
    int refused = 1;
    for (long cut = 1; cut < ss - 1; cut += 7) {
        copy_prefix(sfull, sp, cut);
        char err[256];
        mx_store *bad = mx_store_open(dir, DEFS, T_NCOLL, 10, &HOOKS, err, sizeof err);
        if (bad) { refused = 0; mx_store_close(bad); }
    }
    CHECK(refused);
    copy_prefix(sfull, sp, ss);
    st = open_st(dir, 100);
    CHECK(st && st->seq == 5 && mx_store_count(st, T_ITEMS) == 5);
    mx_store_close(st);
    free(sfull);
    free(sp);
    free(tp);
    free(full);
    free(jp);
    rm_rf(dir);
}

/* A journal write that fails (here: RLIMIT_FSIZE, SIGXFSZ ignored -> EFBIG)
 * must never be followed by a "durable" announcement of the lost records:
 * the store fails for good (read-only), and a reopen shows what is on disk. */
static void t_sticky_write_failure(void) {
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log");
    mx_store *st = open_st(dir, 100);
    st->listener = count_listener;
    listener_calls = 0;
    CHECK(mx_store_put(st, T_ITEMS, item("ok", 1), NULL) == 0 && mx_store_sync(st) == 0 && listener_calls == 1);
    mx_store_health h;
    mx_store_health_get(st, &h);
    CHECK(!h.failed && !mx_store_failed(st) && h.fail_op == NULL && h.synced_seq == 1);
    long before = file_size(jp);

    struct rlimit old_rl, rl;
    CHECK(getrlimit(RLIMIT_FSIZE, &old_rl) == 0);
    void (*old_sig)(int) = signal(SIGXFSZ, SIG_IGN);
    rl = old_rl;
    rl.rlim_cur = (rlim_t)before + 16; /* the next record does not fit */
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);
    json *big = item("lost", 2);
    json_set(big, "pad", json_str("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"));
    CHECK(mx_store_put(st, T_ITEMS, big, NULL) == 0); /* queued; in memory */
    CHECK(mx_store_sync(st) == -1);                   /* write() fails with EFBIG */
    CHECK(setrlimit(RLIMIT_FSIZE, &old_rl) == 0);
    signal(SIGXFSZ, old_sig);
    clearerr(stderr); /* stderr redirected to a file failed too while the limit was set */

    CHECK(mx_store_failed(st));
    mx_store_health_get(st, &h);
    CHECK(h.failed && h.fail_errno == EFBIG && h.fail_op && strcmp(h.fail_op, "write") == 0);
    CHECK(h.synced_seq == 1 && h.seq == 2 && h.pending_records == 0);
    CHECK(listener_calls == 1);
    CHECK(file_size(jp) == before); /* the partial tail was cut */
    /* the bug: the next sync (with room on disk again) announced seq 2 durable */
    CHECK(mx_store_dirty(st) == 0);
    CHECK(mx_store_sync(st) == MX_STORE_EFAILED && listener_calls == 1 && st->synced_seq == 1);
    /* read-only: writes are refused (and make the barrier answer them), reads work */
    errno = 0;
    CHECK(mx_store_put(st, T_ITEMS, item("after", 3), NULL) == -1 && errno == EROFS);
    CHECK(mx_store_del(st, T_ITEMS, "ok", NULL) == -1);
    CHECK(mx_store_dirty(st) == 1);
    CHECK(mx_store_sync(st) == MX_STORE_EFAILED && mx_store_dirty(st) == 0 && listener_calls == 1);
    mx_store_health_get(st, &h);
    CHECK(h.rejected_writes == 2 && h.seq == 2);
    CHECK(mx_store_get(st, T_ITEMS, "ok") && !mx_store_get(st, T_ITEMS, "after"));
    /* the catch-up feed never hands out the lost change */
    sbuf out;
    sbuf_init(&out);
    CHECK(changes_since(st, 0, &out) == 0 && strstr(out.data, "\"ok\"") && !strstr(out.data, "\"lost\""));
    sbuf_free(&out);
    /* no snapshot of memory that holds the lost batch */
    char *sp = file_path(dir, "snapshot.json");
    CHECK(mx_store_compact(st) == -1 && mx_store_compact_bg(st) == -1 && file_size(sp) == -1);
    free(sp);
    mx_store_close(st);
    CHECK(file_size(jp) == before);

    st = open_st(dir, 100);
    CHECK(st && !mx_store_failed(st) && st->seq == 1 && mx_store_get(st, T_ITEMS, "ok") && !mx_store_get(st, T_ITEMS, "lost"));
    CHECK(st && mx_store_put(st, T_ITEMS, item("again", 4), NULL) == 0 && mx_store_sync(st) == 0);
    mx_store_close(st);
    free(jp);
    rm_rf(dir);
}

/* fsyncgate: after a failed fsync nothing is retried or announced. */
static void t_sticky_fsync_failure(void) {
    char *dir = make_tmpdir();
    mx_store *st = open_st(dir, 100);
    st->listener = count_listener;
    listener_calls = 0;
    CHECK(mx_store_put(st, T_ITEMS, item("a", 1), NULL) == 0);
    st->fault_syncs = 1; /* the fsync of this batch fails */
    CHECK(mx_store_sync(st) == -1 && mx_store_failed(st) && listener_calls == 0);
    mx_store_health h;
    mx_store_health_get(st, &h);
    CHECK(h.failed && h.fail_op && strcmp(h.fail_op, "fsync") == 0 && h.synced_seq == 0);
    int64_t fsyncs = st->fsyncs;
    CHECK(mx_store_sync(st) == MX_STORE_EFAILED && st->fsyncs == fsyncs && listener_calls == 0);
    CHECK(mx_store_put(st, T_ITEMS, item("b", 2), NULL) == -1);
    CHECK(mx_store_sync(st) == MX_STORE_EFAILED && listener_calls == 0 && st->synced_seq == 0);
    mx_store_close(st);
    st = open_st(dir, 100);
    CHECK(st && !mx_store_failed(st) && !mx_store_get(st, T_ITEMS, "b"));
    mx_store_close(st);
    rm_rf(dir);
}


/* ---- recovery: only a torn tail is cut; other damage fails the open or is salvaged ---- */

static void append_record(const char *path, const char *body) {
    char hdr[16];
    snprintf(hdr, sizeof hdr, "%08x ", crc32_buf(body, strlen(body)));
    sbuf rec;
    sbuf_init(&rec);
    sbuf_puts(&rec, hdr);
    sbuf_puts(&rec, body);
    sbuf_putc(&rec, '\n');
    append_raw(path, rec.data);
    sbuf_free(&rec);
}

static void append_put(const char *path, int seq, const char *id) {
    char b[256];
    snprintf(b, sizeof b, "{\"seq\":%d,\"entity\":\"items\",\"op\":\"put\",\"id\":\"%s\",\"data\":{\"id\":\"%s\"},\"ts\":1}",
             seq, id, id);
    append_record(path, b);
}

static mx_store *open_opts(const char *dir, int salvage, char *err, size_t errlen) {
    mx_store_opts o = { salvage, NULL };
    return mx_store_open_ex(dir, DEFS, T_NCOLL, 100, &HOOKS, &o, err, errlen);
}

static int count_prefix(const char *dir, const char *prefix) {
    DIR *d = opendir(dir);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) n++;
    if (d) closedir(d);
    return n;
}

static void t_recovery_middle_damage(void) {
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log");
    char err[512];
    mx_store *st;
    long full;
    /* a damaged line in the middle (bad CRC, then not a record at all), valid records after it */
    append_put(jp, 1, "r1");
    append_put(jp, 2, "r2");
    long mid = file_size(jp);
    append_raw(jp, "deadbeef {\"seq\":3,\"entity\":\"items\",\"op\":\"put\",\"id\":\"r3\"}\n");
    append_raw(jp, "not even a record\n");
    append_put(jp, 4, "r4");
    append_put(jp, 5, "r5");
    full = file_size(jp);
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st == NULL && strstr(err, "corrupt record") && strstr(err, "line 3") && strstr(err, "MODX_STORE_SALVAGE"));
    CHECK(file_size(jp) == full);                 /* untouched */
    CHECK(count_prefix(dir, "recovery-") == 0);
    /* the environment variable turns salvage on, too */
    setenv("MODX_STORE_SALVAGE", "1", 1);
    st = open_opts(dir, 0, err, sizeof err);
    unsetenv("MODX_STORE_SALVAGE");
    CHECK(st && st->salvage && st->seq == 5 && mx_store_count(st, T_ITEMS) == 4 && !mx_store_get(st, T_ITEMS, "r3"));
    CHECK(st && mx_store_get(st, T_ITEMS, "r4") && mx_store_get(st, T_ITEMS, "r5"));
    CHECK(st && st->salvaged_records == 2 && st->seq_gaps == 1 && st->recovery_report);
    if (st && st->recovery_report) {
        FILE *f = fopen(st->recovery_report, "r");
        char buf[4096];
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        buf[n] = 0;
        if (f) fclose(f);
        char want[64];
        snprintf(want, sizeof want, "skipped bytes %ld..", mid);
        CHECK(strstr(buf, want) && strstr(buf, "seq gap") && strstr(buf, "skipped records / lines: 2"));
    }
    CHECK(count_prefix(dir, "journal.log.damaged-") == 1 && count_prefix(dir, "recovery-") == 1);
    CHECK(file_size(jp) == 0); /* compacted into a clean snapshot */
    CHECK(st && mx_store_put(st, T_ITEMS, item("r6", 6), NULL) == 0 && mx_store_sync(st) == 0 && st->seq == 6);
    mx_store_close(st);
    /* clean from now on, without salvage */
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st && st->seq == 6 && mx_store_count(st, T_ITEMS) == 5 && !st->recovery_report);
    mx_store_close(st);
    free(jp);
    rm_rf(dir);
}

static void t_recovery_seq(void) {
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log");
    char err[512];
    /* gap: 1, 2, 4 */
    append_put(jp, 1, "a");
    append_put(jp, 2, "b");
    append_put(jp, 4, "d");
    mx_store *st = open_opts(dir, 0, err, sizeof err);
    CHECK(st == NULL && strstr(err, "seq gap") && strstr(err, "seq 4 after 2"));
    /* a journal that starts after the snapshot's seq + 1 has a gap as well */
    unlink(jp);
    append_put(jp, 3, "c");
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st == NULL && strstr(err, "seq 3 after 0"));
    /* a CRC-valid put whose row has no "id" is damage, not a row (it used to be stored, and the next
     * snapshot crashed sorting it) */
    unlink(jp);
    append_put(jp, 1, "a");
    append_record(jp, "{\"seq\":2,\"entity\":\"items\",\"op\":\"put\",\"id\":\"x\",\"data\":{\"n\":1},\"ts\":1}");
    append_put(jp, 3, "c");
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st == NULL && strstr(err, "without an id"));
    st = open_opts(dir, 1, err, sizeof err);
    CHECK(st && st->seq == 3 && mx_store_count(st, T_ITEMS) == 2 && !mx_store_get(st, T_ITEMS, "x"));
    mx_store_close(st);
    rm_rf(dir);
    dir = make_tmpdir();
    free(jp);
    jp = file_path(dir, "journal.log");
    /* regression: 1, 2, 2 */
    unlink(jp);
    append_put(jp, 1, "a");
    append_put(jp, 2, "b");
    append_put(jp, 2, "b2");
    append_put(jp, 3, "c");
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st == NULL && strstr(err, "backwards"));
    st = open_opts(dir, 1, err, sizeof err);
    CHECK(st && st->seq == 3 && !mx_store_get(st, T_ITEMS, "b2") && mx_store_get(st, T_ITEMS, "c") &&
          st->salvaged_records == 1);
    mx_store_close(st);
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st && st->seq == 3 && mx_store_count(st, T_ITEMS) == 3);
    mx_store_close(st);
    /* records the snapshot already covers are skipped (a truncate that failed after a compaction) */
    char *sp = file_path(dir, "snapshot.json");
    unlink(sp);
    unlink(jp);
    append_raw(sp, "{\"seq\":2,\"collections\":{\"items\":[{\"id\":\"a\"},{\"id\":\"b\"}]}}\n");
    append_put(jp, 1, "a");
    append_put(jp, 2, "b");
    append_put(jp, 3, "c");
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st && st->seq == 3 && mx_store_count(st, T_ITEMS) == 3);
    mx_store_close(st);
    free(sp);
    free(jp);
    rm_rf(dir);
}

/* journal.old was fsynced before the rotation: any damage in it is fatal (it used to stop there quietly
 * and go on with journal.log, dropping the rest of journal.old) */
static void t_recovery_journal_old(void) {
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log"), *op = file_path(dir, "journal.old");
    char err[512];
    append_put(op, 1, "a");
    append_raw(op, "00000000 torn\n");
    append_put(op, 2, "b");
    append_put(jp, 3, "c");
    mx_store *st = open_opts(dir, 0, err, sizeof err);
    CHECK(st == NULL && strstr(err, "journal.old: corrupt record"));
    unlink(op);
    append_put(op, 1, "a");
    append_put(op, 2, "b");
    append_raw(op, "00000000 {\"seq\":3");
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st == NULL && strstr(err, "journal.old") && strstr(err, "never torn"));
    st = open_opts(dir, 1, err, sizeof err);
    CHECK(st && st->seq == 3 && mx_store_count(st, T_ITEMS) == 3 && file_size(op) == -1);
    CHECK(count_prefix(dir, "journal.old.damaged-") == 1);
    mx_store_close(st);
    free(jp);
    free(op);
    rm_rf(dir);
}

/* Snapshots carry a CRC; old ones (0.1 / 0.2, without it) still open. */
static void t_snapshot_checksum(void) {
    char *dir = make_tmpdir();
    char *jp = file_path(dir, "journal.log"), *sp = file_path(dir, "snapshot.json");
    char err[512];
    /* the 0.2 format: plain snapshot, CRC journal */
    append_raw(sp, "{\"seq\":2,\"collections\":{\"items\":[\n{\"id\":\"a\",\"n\":1},\n{\"id\":\"b\",\"n\":2}],\"labels\":[]}}\n");
    append_put(jp, 3, "c");
    mx_store *st = open_opts(dir, 0, err, sizeof err);
    CHECK(st && st->seq == 3 && mx_store_count(st, T_ITEMS) == 3 && !st->recovered_truncated);
    CHECK(st && mx_store_compact(st) == 0);
    mx_store_close(st);
    FILE *f = fopen(sp, "rb");
    char head[48] = { 0 };
    CHECK(f && fread(head, 1, 33, f) == 33);
    if (f) fclose(f);
    CHECK(strncmp(head, "{\"crc32\":\"", 10) == 0 && head[18] == '/' && head[31] == '"' && head[32] == ',');
    CHECK(atol(head + 19) == file_size(sp) - 33);
    /* the new snapshot is still plain JSON (an older reader sees one more key) */
    {
        FILE *g = fopen(sp, "rb");
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf, g);
        fclose(g);
        json *j = json_parse(buf, n, NULL, 0);
        CHECK(j && json_get_num(j, "seq", 0) == 3 && json_get_str(j, "crc32"));
        json_free(j);
    }
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st && st->seq == 3 && mx_store_count(st, T_ITEMS) == 3);
    mx_store_close(st);
    /* a flipped bit inside a string still parses, but fails the checksum */
    long ss = file_size(sp);
    flip_bit(sp, ss - 10, 0x01);
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st == NULL && strstr(err, "corrupt snapshot") && strstr(err, "checksum mismatch"));
    st = open_opts(dir, 1, err, sizeof err);
    CHECK(st && st->snapshot_crc_mismatch && st->recovery_report && count_prefix(dir, "snapshot.json.damaged-") == 1);
    mx_store_close(st);
    st = open_opts(dir, 0, err, sizeof err); /* rewritten by the salvage */
    CHECK(st && st->seq == 3 && !st->snapshot_unchecked);
    mx_store_close(st);
    /* an operator re-serializes it with another tool (keeps the stale "crc32" key, changes the length):
     * loaded without the check, not refused */
    {
        FILE *g = fopen(sp, "rb");
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf, g);
        fclose(g);
        json *j = json_parse(buf, n, NULL, 0);
        json *items = json_get(json_get(j, "collections"), "items");
        CHECK(items && items->len == 3);
        if (items && items->len) json_set(items->items[0], "n", json_num(42));
        sbuf w;
        sbuf_init(&w);
        json_write(&w, j);
        json_free(j);
        unlink(sp);
        append_raw(sp, w.data);
        sbuf_free(&w);
    }
    st = open_opts(dir, 0, err, sizeof err);
    CHECK(st && st->snapshot_unchecked && st->seq == 3 && json_get_num(mx_store_get(st, T_ITEMS, "a"), "n", 0) == 42);
    mx_store_close(st);
    free(jp);
    free(sp);
    rm_rf(dir);
}

void test_store_all(void) {
    RUN(t_hmap);
    RUN(t_append_reload);
    RUN(t_torn_tail);
    RUN(t_crc_mismatch);
    RUN(t_unknown_collection);
    RUN(t_compaction);
    RUN(t_ring_window);
    RUN(t_flags_hooks);
    RUN(t_background_compaction);
    RUN(t_indexes);
    RUN(t_crash_points);
    RUN(t_sticky_write_failure);
    RUN(t_sticky_fsync_failure);
    RUN(t_recovery_middle_damage);
    RUN(t_recovery_seq);
    RUN(t_recovery_journal_old);
    RUN(t_snapshot_checksum);
}
