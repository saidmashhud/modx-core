/* Store: hash map, journal append + reload, torn tails and CRC mismatches,
 * compaction, corrupt snapshots, the change ring and its window, listener
 * only after fsync, collection flags (private / no ring / redact), the
 * row_key / journal_extra / scope / on_apply hooks. */
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "modx/store.h"
#include "test.h"
#include "test_util.h"

enum { T_ITEMS, T_LABELS, T_SECRETS, T_USERS, T_BULK, T_NCOLL };
static const mx_coll_def DEFS[T_NCOLL] = {
    { "items", 0 },
    { "labels", 0 },
    { "secrets", MX_COLL_PRIVATE },
    { "users", MX_COLL_REDACT },
    { "bulk", MX_COLL_PRIVATE | MX_COLL_NO_RING },
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

void test_store_all(void) {
    RUN(t_hmap);
    RUN(t_append_reload);
    RUN(t_torn_tail);
    RUN(t_crc_mismatch);
    RUN(t_compaction);
    RUN(t_ring_window);
    RUN(t_flags_hooks);
    RUN(t_background_compaction);
}
