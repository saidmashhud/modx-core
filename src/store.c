#include "modx/store.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---------------- hash map ---------------- */

static uint64_t hash_str(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

void hmap_init(hmap *m) { m->slots = NULL; m->cap = 0; m->count = 0; }

void hmap_free(hmap *m) {
    for (size_t i = 0; i < m->cap; i++) {
        if (m->slots[i].key) { free(m->slots[i].key); json_free(m->slots[i].val); }
    }
    free(m->slots);
    hmap_init(m);
}

static size_t hmap_find(const hmap *m, const char *key) {
    size_t mask = m->cap - 1;
    size_t i = (size_t)hash_str(key) & mask;
    while (m->slots[i].key && strcmp(m->slots[i].key, key) != 0) i = (i + 1) & mask;
    return i;
}

static void hmap_grow(hmap *m) {
    size_t ncap = m->cap ? m->cap * 2 : 64;
    hentry *old = m->slots;
    size_t ocap = m->cap;
    m->slots = xcalloc(ncap, sizeof *m->slots);
    m->cap = ncap;
    for (size_t i = 0; i < ocap; i++) {
        if (!old[i].key) continue;
        size_t j = hmap_find(m, old[i].key);
        m->slots[j] = old[i];
    }
    free(old);
}

json *hmap_get(const hmap *m, const char *key) {
    if (!m->cap) return NULL;
    size_t i = hmap_find(m, key);
    return m->slots[i].key ? m->slots[i].val : NULL;
}

json *hmap_swap(hmap *m, const char *key, json *val) {
    if ((m->count + 1) * 4 >= m->cap * 3) hmap_grow(m);
    size_t i = hmap_find(m, key);
    if (m->slots[i].key) {
        json *old = m->slots[i].val;
        m->slots[i].val = val;
        return old;
    }
    m->slots[i].key = xstrdup(key);
    m->slots[i].val = val;
    m->count++;
    return NULL;
}

void hmap_put(hmap *m, const char *key, json *val) { json_free(hmap_swap(m, key, val)); }

json *hmap_take(hmap *m, const char *key) {
    if (!m->cap) return NULL;
    size_t mask = m->cap - 1;
    size_t i = hmap_find(m, key);
    if (!m->slots[i].key) return NULL;
    json *val = m->slots[i].val;
    free(m->slots[i].key);
    m->slots[i].key = NULL;
    m->slots[i].val = NULL;
    m->count--;
    /* backward-shift deletion keeps probe chains intact without tombstones */
    size_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!m->slots[j].key) break;
        size_t home = (size_t)hash_str(m->slots[j].key) & mask;
        /* can slot j's entry move to the hole at i? (home not cyclically in (i, j]) */
        int move = (i <= j) ? (home <= i || home > j) : (home <= i && home > j);
        if (move) {
            m->slots[i] = m->slots[j];
            m->slots[j].key = NULL;
            m->slots[j].val = NULL;
            i = j;
        }
    }
    return val ? val : json_null(); /* never NULL for a removed key */
}

int hmap_del(hmap *m, const char *key) {
    json *v = hmap_take(m, key);
    if (!v) return 0;
    json_free(v);
    return 1;
}

/* ---------------- secondary indexes ----------------
 * value -> bucket (an open-addressing set of storage keys). Buckets are
 * found by open addressing on the value, keys inside a bucket likewise. */

struct mx_ixbucket {
    char *value;
    char **keys;      /* open addressing; NULL = empty */
    size_t cap, count;
};

static size_t set_find(char **keys, size_t cap, const char *k) {
    size_t mask = cap - 1, i = (size_t)hash_str(k) & mask;
    while (keys[i] && strcmp(keys[i], k) != 0) i = (i + 1) & mask;
    return i;
}

static void bucket_add(mx_ixbucket *b, const char *key) {
    if ((b->count + 1) * 4 >= b->cap * 3) {
        size_t ncap = b->cap ? b->cap * 2 : 8;
        char **nk = xcalloc(ncap, sizeof *nk);
        for (size_t i = 0; i < b->cap; i++)
            if (b->keys[i]) nk[set_find(nk, ncap, b->keys[i])] = b->keys[i];
        free(b->keys);
        b->keys = nk;
        b->cap = ncap;
    }
    size_t i = set_find(b->keys, b->cap, key);
    if (b->keys[i]) return;
    b->keys[i] = xstrdup(key);
    b->count++;
}

static void bucket_remove(mx_ixbucket *b, const char *key) {
    if (!b->cap) return;
    size_t mask = b->cap - 1, i = set_find(b->keys, b->cap, key);
    if (!b->keys[i]) return;
    free(b->keys[i]);
    b->keys[i] = NULL;
    b->count--;
    size_t j = i;
    for (;;) { /* backward-shift deletion */
        j = (j + 1) & mask;
        if (!b->keys[j]) break;
        size_t home = (size_t)hash_str(b->keys[j]) & mask;
        int move = (i <= j) ? (home <= i || home > j) : (home <= i && home > j);
        if (move) { b->keys[i] = b->keys[j]; b->keys[j] = NULL; i = j; }
    }
}

static size_t ix_slot(const mx_index *ix, const char *value) {
    size_t mask = ix->cap - 1, i = (size_t)hash_str(value) & mask;
    while (ix->slots[i] && strcmp(ix->slots[i]->value, value) != 0) i = (i + 1) & mask;
    return i;
}

static mx_ixbucket *ix_get(const mx_index *ix, const char *value) {
    if (!ix->cap) return NULL;
    return ix->slots[ix_slot(ix, value)];
}

static mx_ixbucket *ix_get_or_add(mx_index *ix, const char *value) {
    if ((ix->count + 1) * 4 >= ix->cap * 3) {
        size_t ncap = ix->cap ? ix->cap * 2 : 64;
        mx_ixbucket **old = ix->slots;
        size_t ocap = ix->cap;
        ix->slots = xcalloc(ncap, sizeof *ix->slots);
        ix->cap = ncap;
        for (size_t i = 0; i < ocap; i++)
            if (old[i]) ix->slots[ix_slot(ix, old[i]->value)] = old[i];
        free(old);
    }
    size_t i = ix_slot(ix, value);
    if (!ix->slots[i]) {
        ix->slots[i] = xcalloc(1, sizeof **ix->slots);
        ix->slots[i]->value = xstrdup(value);
        ix->count++;
    }
    return ix->slots[i];
}

/* Empty buckets are kept (values usually come back); freed with the index. */
static void ix_free(mx_index *ix) {
    for (size_t i = 0; i < ix->cap; i++) {
        mx_ixbucket *b = ix->slots[i];
        if (!b) continue;
        for (size_t k = 0; k < b->cap; k++) free(b->keys[k]);
        free(b->keys);
        free(b->value);
        free(b);
    }
    free(ix->slots);
}

/* Keep every index of coll in line with a row change (old -> row, either NULL). */
static void ix_update(mx_store *st, int coll, const char *key, const json *old, const json *row) {
    for (int i = 0; i < st->nindexes; i++) {
        mx_index *ix = &st->indexes[i];
        if (ix->coll != coll) continue;
        const char *a = old ? json_get_str(old, ix->field) : NULL;
        const char *b = row ? json_get_str(row, ix->field) : NULL;
        if (a && b && strcmp(a, b) == 0) continue;
        if (a) { mx_ixbucket *bk = ix_get(ix, a); if (bk) bucket_remove(bk, key); }
        if (b) bucket_add(ix_get_or_add(ix, b), key);
    }
}

static const mx_index *ix_for(const mx_store *st, int coll, const char *field) {
    for (int i = 0; i < st->nindexes; i++)
        if (st->indexes[i].coll == coll && strcmp(st->indexes[i].field, field) == 0) return &st->indexes[i];
    return NULL;
}

int mx_store_indexed(const mx_store *st, int coll, const char *field) { return ix_for(st, coll, field) != NULL; }

const json **mx_store_find(const mx_store *st, int coll, const char *field, const char *value, size_t *n) {
    *n = 0;
    if (coll < 0 || coll >= st->ncoll || !field || !value) return xmalloc(sizeof(json *));
    const mx_index *ix = ix_for(st, coll, field);
    if (ix) {
        const mx_ixbucket *b = ix_get(ix, value);
        const json **out = xmalloc(((b && b->count) ? b->count : 1) * sizeof *out);
        if (b)
            for (size_t i = 0; i < b->cap; i++)
                if (b->keys[i]) {
                    const json *r = hmap_get(&st->colls[coll], b->keys[i]);
                    if (r) out[(*n)++] = r;
                }
        return out;
    }
    const hmap *m = &st->colls[coll];
    const json **out = xmalloc((m->count ? m->count : 1) * sizeof *out);
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->slots[i].key) continue;
        const char *v = json_get_str(m->slots[i].val, field);
        if (v && strcmp(v, value) == 0) out[(*n)++] = m->slots[i].val;
    }
    return out;
}

size_t mx_store_find_count(const mx_store *st, int coll, const char *field, const char *value) {
    const mx_index *ix = coll >= 0 && coll < st->ncoll && field && value ? ix_for(st, coll, field) : NULL;
    if (ix) {
        const mx_ixbucket *b = ix_get(ix, value);
        return b ? b->count : 0;
    }
    size_t n;
    free(mx_store_find(st, coll, field, value, &n));
    return n;
}

/* ---------------- file helpers ---------------- */

static char *path_join(const char *a, const char *b) {
    sbuf s;
    sbuf_init(&s);
    sbuf_puts(&s, a);
    if (s.len && s.data[s.len - 1] != '/') sbuf_putc(&s, '/');
    sbuf_puts(&s, b);
    return sbuf_take(&s);
}

static int mkdir_p(const char *path) {
    char *tmp = xstrdup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
            *p = '/';
        }
    }
    int rc = (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
    free(tmp);
    return rc;
}

static int read_file(const char *path, sbuf *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat stt;
    if (fstat(fd, &stt) == 0 && stt.st_size > 0) sbuf_reserve_exact(out, (size_t)stt.st_size);
    for (;;) {
        sbuf_reserve(out, 65536);
        ssize_t n = read(fd, out->data + out->len, out->cap - out->len - 1);
        if (n < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (n == 0) break;
        out->len += (size_t)n;
        out->data[out->len] = 0;
    }
    close(fd);
    if (!out->data) sbuf_reserve(out, 0);
    return 0;
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int fsync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

static int flush_pending(mx_store *st);
static void compact_done(mx_store *st, int status);
static void store_fail(mx_store *st, const char *op, int err);

/* ---------------- ring ---------------- */

static void change_free(mx_store *st, mx_change *e) {
    free(e->json);
    if (e->scope && st->hooks.scope_free) st->hooks.scope_free(st->hooks.ctx, e->scope);
    memset(e, 0, sizeof *e);
}

const mx_change *mx_store_ring_at(const mx_store *st, size_t i) {
    return &st->ring[(st->ring_start + i) % st->ring_cap];
}

/* Takes ownership of js. Returns the slot (scope still to be set). */
static mx_change *ring_push(mx_store *st, int64_t seq, int coll, char *js) {
    if (!st->ring_cap) { free(js); return NULL; }
    size_t idx;
    if (st->ring_len == st->ring_cap) {
        idx = st->ring_start;
        change_free(st, &st->ring[idx]);
        st->ring_start = (st->ring_start + 1) % st->ring_cap;
    } else {
        idx = (st->ring_start + st->ring_len) % st->ring_cap;
        st->ring_len++;
    }
    mx_change *e = &st->ring[idx];
    e->seq = seq;
    e->coll = coll;
    e->json = js;
    e->scope = NULL;
    return e;
}

/* ---------------- serialization ---------------- */

static unsigned coll_flags(const mx_store *st, int coll) { return st->defs[coll].flags; }

static void write_row_public(const mx_store *st, int coll, const json *row, sbuf *out) {
    if ((coll_flags(st, coll) & MX_COLL_REDACT) && st->hooks.write_public)
        st->hooks.write_public(st->hooks.ctx, coll, row, out);
    else
        json_write(out, row);
}

/* {"seq","entity","op","id","data","client_id"?,<extra>,"ts"}; public: redacted, no extra. */
static void write_change(const mx_store *st, sbuf *ch, int64_t seq, int coll, const char *op, const char *id,
                         const char *key, const json *data, const char *client_id, int64_t ts, int public,
                         int *had_extra) {
    sbuf_printf(ch, "{\"seq\":%lld,\"entity\":", (long long)seq);
    json_write_string(ch, st->defs[coll].name, strlen(st->defs[coll].name));
    sbuf_printf(ch, ",\"op\":\"%s\",\"id\":", op);
    json_write_string(ch, id, strlen(id));
    sbuf_puts(ch, ",\"data\":");
    if (public) write_row_public(st, coll, data, ch);
    else json_write(ch, data);
    if (client_id) {
        sbuf_puts(ch, ",\"client_id\":");
        json_write_string(ch, client_id, strlen(client_id));
    }
    if (!public && st->hooks.journal_extra) {
        size_t before = ch->len;
        st->hooks.journal_extra(st->hooks.ctx, coll, id, key, data, ch);
        if (had_extra) *had_extra = ch->len != before;
    }
    sbuf_printf(ch, ",\"ts\":%lld}", (long long)ts);
}

static char *key_for(const mx_store *st, int coll, const char *id, const json *rec, const json *row) {
    char *k = st->hooks.row_key ? st->hooks.row_key(st->hooks.ctx, coll, id, rec, row) : NULL;
    return k ? k : xstrdup(id);
}

/* ---------------- apply ---------------- */

/* Store (row != NULL, ownership taken) or remove the row under key, then run on_apply. */
static void apply(mx_store *st, int coll, const char *key, json *row) {
    hmap *m = &st->colls[coll];
    json *old = row ? hmap_swap(m, key, row) : hmap_take(m, key);
    if (st->nindexes) ix_update(st, coll, key, old && old->type == JSON_OBJECT ? old : NULL, row);
    if (st->hooks.on_apply) st->hooks.on_apply(st->hooks.ctx, st, coll, key, old, row);
    json_free(old);
}

static void record_change(mx_store *st, int64_t seq, int coll, const char *op, const char *id, const char *key,
                          const json *row, const char *client_id, int64_t ts, int replay, char *pub) {
    if (coll_flags(st, coll) & MX_COLL_NO_RING) { free(pub); return; }
    mx_commit_info ci = { seq, coll, op, id, key, hmap_get(&st->colls[coll], key), row, client_id, ts, replay, pub };
    void *scope = st->hooks.scope_new ? st->hooks.scope_new(st->hooks.ctx, st, &ci) : NULL;
    mx_change *e = ring_push(st, seq, coll, pub);
    if (e) e->scope = scope;
    else if (scope && st->hooks.scope_free) st->hooks.scope_free(st->hooks.ctx, scope);
}

/* ---------------- open / load ---------------- */

int mx_store_coll(const mx_store *st, const char *name) {
    if (!name) return -1;
    for (int i = 0; i < st->ncoll; i++)
        if (strcmp(st->defs[i].name, name) == 0) return i;
    return -1;
}

const char *mx_store_coll_name(const mx_store *st, int coll) {
    return coll >= 0 && coll < st->ncoll ? st->defs[coll].name : NULL;
}

/* Recovery state carried across the replayed files (journal.old, then journal.log). */
typedef struct {
    int salvage;          /* skip damaged records / accept seq gaps instead of failing */
    int64_t snap_seq;     /* seq of the loaded snapshot */
    int64_t last;         /* last seq taken (snap_seq before the first record past the snapshot) */
    int past_snapshot;    /* a record with seq > snap_seq was taken */
    sbuf report;          /* salvage report lines */
    int damaged;          /* salvage skipped something (the files are kept as *.damaged-<ms>) */
    int64_t stamp;        /* now_ms() of this open: names the report and the kept copies */
} replay_state;

typedef struct {
    size_t off, len; /* line without its '\n' */
    int ok;          /* frame (CRC) valid */
} line_span;

static void report_line(replay_state *rs, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void report_line(replay_state *rs, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    sbuf_puts(&rs->report, buf);
    sbuf_putc(&rs->report, '\n');
    fprintf(stderr, "modx: salvage: %s\n", buf);
}

static const char *base_name(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* Keeps a copy of a damaged file's bytes next to it before salvage rewrites the store. */
static int keep_damaged_copy(const char *path, const sbuf *bytes, replay_state *rs) {
    size_t cap = strlen(path) + 48;
    char *copy = xmalloc(cap);
    int fd = -1;
    for (int k = 0; k < 100 && fd < 0; k++) { /* never overwrite an earlier copy */
        if (k) snprintf(copy, cap, "%s.damaged-%lld-%d", path, (long long)rs->stamp, k);
        else snprintf(copy, cap, "%s.damaged-%lld", path, (long long)rs->stamp);
        fd = open(copy, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0 && errno != EEXIST) break;
    }
    int rc = fd >= 0 && write_all(fd, bytes->data, bytes->len) == 0 && fsync(fd) == 0 ? 0 : -1;
    if (fd >= 0) close(fd);
    if (rc == 0) report_line(rs, "original bytes of %s kept as %s", base_name(path), base_name(copy));
    free(copy);
    return rc;
}

/* Snapshot files start with {"crc32":"<hex8>/<len12>", : the CRC-32 and the
 * byte length of everything after that prefix (the rest of the object). A
 * bit flip keeps the length and fails the CRC. A file rewritten by another
 * tool (an operator's jq / JSON.stringify edit) almost always has another
 * length: it is loaded without the check, with a warning. A truncated one
 * does not parse. Old readers see one more key; snapshots without it
 * (written before 0.3) are read without a check. */
#define SNAP_CRC_PREFIX "{\"crc32\":\""
#define SNAP_CRC_HDR 33 /* {"crc32":"XXXXXXXX/LLLLLLLLLLLL", */

/* 1: the snapshot carries a checksum header (*want, *want_len set); 0: old format. */
static int snapshot_crc(const char *data, size_t len, uint32_t *want, uint64_t *want_len) {
    if (len < SNAP_CRC_HDR + 1 || memcmp(data, SNAP_CRC_PREFIX, 10) != 0 || data[18] != '/' || data[31] != '"' ||
        data[32] != ',')
        return 0;
    uint32_t v = 0;
    for (int i = 10; i < 18; i++) {
        char c = data[i];
        int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
        if (h < 0) return 0;
        v = (v << 4) | (uint32_t)h;
    }
    uint64_t l = 0;
    for (int i = 19; i < 31; i++) {
        if (data[i] < '0' || data[i] > '9') return 0;
        l = l * 10 + (uint64_t)(data[i] - '0');
    }
    *want = v;
    *want_len = l;
    return 1;
}

static int load_snapshot(mx_store *st, replay_state *rs, char *err, size_t errlen) {
    sbuf s;
    sbuf_init(&s);
    if (read_file(st->snapshot_path, &s) != 0) {
        sbuf_free(&s);
        if (errno == ENOENT) return 0;
        snprintf(err, errlen, "cannot read %s: %s", st->snapshot_path, strerror(errno));
        return -1;
    }
    if (s.len == 0) { sbuf_free(&s); return 0; }
    uint32_t want;
    uint64_t want_len;
    int has_crc = snapshot_crc(s.data, s.len, &want, &want_len);
    size_t body_len = has_crc ? s.len - SNAP_CRC_HDR : 0;
    int len_differs = has_crc && want_len != (uint64_t)body_len;
    if (has_crc && !len_differs) {
        uint32_t got = crc32_buf(s.data + SNAP_CRC_HDR, s.len - SNAP_CRC_HDR);
        if (got != want) {
            if (!rs->salvage) {
                snprintf(err, errlen, "corrupt snapshot %s: checksum mismatch (stored %08x, computed %08x)",
                         st->snapshot_path, want, got);
                sbuf_free(&s);
                return -1;
            }
            /* salvage: use it if it still parses (it is never silently half-loaded) */
            report_line(rs, "snapshot.json: checksum mismatch (stored %08x, computed %08x); loaded because it parses",
                        want, got);
            rs->damaged = 1;
            st->snapshot_crc_mismatch = 1;
            if (keep_damaged_copy(st->snapshot_path, &s, rs) != 0) {
                snprintf(err, errlen, "salvage: cannot keep a copy of %s: %s", st->snapshot_path, strerror(errno));
                sbuf_free(&s);
                return -1;
            }
        }
    }
    char perr[128];
    json *root = json_parse(s.data, s.len, perr, sizeof perr);
    sbuf_free(&s);
    if (!root || root->type != JSON_OBJECT) {
        if (len_differs)
            snprintf(err, errlen, "corrupt snapshot %s: %s (%zu bytes after the checksum header, which says %llu: truncated?)",
                     st->snapshot_path, root ? "not an object" : perr, body_len, (unsigned long long)want_len);
        else
            snprintf(err, errlen, "corrupt snapshot %s: %s", st->snapshot_path, root ? "not an object" : perr);
        json_free(root);
        return -1;
    }
    if (len_differs) {
        /* parses, but not the bytes modx wrote: an edit by another tool */
        fprintf(stderr, "modx: %s was rewritten outside modx (%zu bytes after its checksum header, which says %llu); "
                        "loaded without the checksum\n", st->snapshot_path, body_len, (unsigned long long)want_len);
        st->snapshot_unchecked = 1;
    }
    st->seq = (int64_t)json_get_num(root, "seq", 0);
    json *colls = json_get(root, "collections");
    if (colls && colls->type == JSON_OBJECT) {
        for (size_t i = 0; i < colls->len; i++) {
            int c = mx_store_coll(st, colls->keys[i]);
            json *v = colls->items[i];
            if (c < 0 || (v->type != JSON_ARRAY && v->type != JSON_OBJECT)) continue;
            for (size_t k = 0; k < v->len; k++) {
                json *obj = v->items[k];
                const char *id = json_get_str(obj, "id");
                if (obj->type != JSON_OBJECT || !id) continue;
                char *key = key_for(st, c, id, NULL, obj);
                v->items[k] = json_null(); /* steal the object out of the parsed tree */
                json *prev = hmap_swap(&st->colls[c], key, obj);
                if (st->nindexes) ix_update(st, c, key, prev, obj);
                json_free(prev);
                free(key);
            }
        }
    }
    json_free(root);
    if (st->hooks.on_apply) {
        /* derived state sees every loaded row once (st->loading tells the hook) */
        st->loading = 1;
        for (int c = 0; c < st->ncoll; c++) {
            hmap *m = &st->colls[c];
            for (size_t i = 0; i < m->cap; i++)
                if (m->slots[i].key) st->hooks.on_apply(st->hooks.ctx, st, c, m->slots[i].key, NULL, m->slots[i].val);
        }
        st->loading = 0;
    }
    return 0;
}

/* Checks the "<crc8> " frame of one line (without its '\n'). 1 = the CRC matches. */
static int frame_ok(const char *line, size_t line_len) {
    if (line_len < 10 || line[8] != ' ') return 0;
    uint32_t want = 0;
    for (int i = 0; i < 8; i++) {
        char c = line[i];
        int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (h < 0) return 0;
        want = (want << 4) | (uint32_t)h;
    }
    return crc32_buf(line + 9, line_len - 9) == want;
}

/* The JSON of a line whose frame is valid, or NULL (not a change record). */
static json *parse_record_body(const char *line, size_t line_len, const char **why) {
    json *ch = json_parse(line + 9, line_len - 9, NULL, 0);
    if (!ch || ch->type != JSON_OBJECT || !json_is_int(json_get(ch, "seq"))) {
        json_free(ch);
        *why = "checksum matches but the record is not a change object";
        return NULL;
    }
    const char *op = json_get_str(ch, "op"), *cid = json_get_str(ch, "id");
    const json *data = json_get(ch, "data");
    int is_put = op && strcmp(op, "put") == 0, is_del = op && strcmp(op, "del") == 0;
    if (!cid || (!is_put && !is_del) || (is_put && (!data || data->type != JSON_OBJECT || !json_get_str(data, "id")))) {
        /* a put's row needs its string "id" like every stored row (mx_store_put requires it) */
        json_free(ch);
        *why = "checksum matches but the record has no id / op / data (or a row without an id)";
        return NULL;
    }
    return ch;
}

/* Applies one valid record (seq order already checked). Takes ch. */
static void replay_record(mx_store *st, json *ch, int64_t seq) {
    int coll = mx_store_coll(st, json_get_str(ch, "entity"));
    if (coll < 0) {
        /* a well-formed record (its CRC matched) of a collection this build does not know:
         * written by a newer version. Skip it, but keep its seq (a rollback must not lose
         * the records after it). */
        st->seq = seq;
        st->skipped_records++;
        json_free(ch);
        return;
    }
    const char *op = json_get_str(ch, "op"), *cid = json_get_str(ch, "id");
    json *data = json_get(ch, "data");
    if (data && data->type != JSON_OBJECT) data = NULL;
    char *key = key_for(st, coll, cid, ch, data);
    const char *client_id = json_get_str(ch, "client_id");
    int64_t ts = (int64_t)json_get_num(ch, "ts", 0);
    char *pub = NULL;
    if (!(coll_flags(st, coll) & (MX_COLL_PRIVATE | MX_COLL_NO_RING))) {
        sbuf p;
        sbuf_init(&p);
        write_change(st, &p, seq, coll, op, cid, key, data, client_id, ts, 1, NULL);
        pub = sbuf_take(&p);
    }
    record_change(st, seq, coll, op, cid, key, data, client_id, ts, 1, pub);
    apply(st, coll, key, data ? json_clone(data) : NULL);
    free(key);
    st->seq = seq;
    json_free(ch);
}

/* Replays one journal file.
 *
 * Every line is "<crc32 hex8> <json>\n". Only a torn TAIL is expected after a
 * crash: the bytes after the last record whose CRC matches (an incomplete
 * record, a record without its '\n', garbage the file system left). In the
 * live journal (live = 1) that tail is cut off and reported. Anything else is
 * damage that a crash cannot produce: a bad record with valid records after
 * it, a seq gap or a seq going backwards, any bad byte in journal.old (it was
 * fsynced before it was rotated). Then open fails, because replaying past it
 * would silently lose data, unless rs->salvage is set: the valid records
 * before and after the damage are kept, every skipped range is reported.
 * Records with seq <= the snapshot's are covered by it and only skipped.
 * Returns the byte length of the file once a torn tail is cut, or -1. */
static int64_t replay_file(mx_store *st, const char *path, int live, replay_state *rs, char *err, size_t errlen) {
    sbuf s;
    sbuf_init(&s);
    if (read_file(path, &s) != 0) {
        sbuf_free(&s);
        if (errno == ENOENT) return 0;
        snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    const char *name = base_name(path);
    /* pass 1: split into lines, check every frame, find the end of the last valid one */
    line_span *ls = NULL;
    size_t nls = 0, cap = 0, pos = 0, good_end = 0, nvalid_end = 0;
    while (pos < s.len) {
        char *nl = memchr(s.data + pos, '\n', s.len - pos);
        if (!nl) break; /* no terminating newline: torn (only ever the last line) */
        size_t len = (size_t)(nl - (s.data + pos));
        if (nls == cap) { cap = cap ? cap * 2 : 256; ls = xrealloc(ls, cap * sizeof *ls); }
        ls[nls].off = pos;
        ls[nls].len = len;
        ls[nls].ok = frame_ok(s.data + pos, len);
        nls++;
        pos += len + 1;
        if (ls[nls - 1].ok) { good_end = pos; nvalid_end = nls; }
    }
    /* pass 2: the lines up to the last valid one; damage here is not a torn tail */
    int64_t rc = 0;
    int fatal = 0, damaged = 0;
    for (size_t i = 0; i < nvalid_end && !fatal; i++) {
        const char *line = s.data + ls[i].off;
        const char *why = "bad checksum";
        json *ch = ls[i].ok ? parse_record_body(line, ls[i].len, &why) : NULL;
        if (!ch) {
            /* a run of bad lines, then valid records */
            size_t j = i;
            while (j + 1 < nvalid_end && !ls[j + 1].ok) j++;
            size_t bad_end = ls[j].off + ls[j].len + 1;
            if (!rs->salvage) {
                snprintf(err, errlen,
                         "%s: corrupt record at byte %zu (line %zu: %s), followed by valid records up to byte %zu; "
                         "refusing to open (that would drop them). Open with salvage (mx_store_opts.salvage or "
                         "MODX_STORE_SALVAGE=1) to keep every valid record and skip the damage",
                         name, ls[i].off, i + 1, why, good_end);
                fatal = 1;
                break;
            }
            report_line(rs, "%s: skipped bytes %zu..%zu (lines %zu..%zu, %zu line(s)): %s", name, ls[i].off, bad_end,
                        i + 1, j + 1, j - i + 1, why);
            st->salvaged_records += (int64_t)(j - i + 1);
            st->salvaged_bytes += (int64_t)(bad_end - ls[i].off);
            damaged = 1;
            i = j;
            continue;
        }
        int64_t seq = (int64_t)json_get(ch, "seq")->number;
        if (!rs->past_snapshot && seq <= rs->snap_seq) { json_free(ch); continue; } /* in the snapshot */
        if (seq != rs->last + 1) {
            int back = seq <= rs->last;
            if (!rs->salvage) {
                snprintf(err, errlen,
                         back ? "%s: seq goes backwards at byte %zu (line %zu): seq %lld after %lld; refusing to open. "
                                "Open with salvage (mx_store_opts.salvage or MODX_STORE_SALVAGE=1) to skip such records"
                              : "%s: seq gap at byte %zu (line %zu): seq %lld after %lld (records missing); refusing "
                                "to open. Open with salvage (mx_store_opts.salvage or MODX_STORE_SALVAGE=1) to accept "
                                "the gap",
                         name, ls[i].off, i + 1, (long long)seq, (long long)rs->last);
                json_free(ch);
                fatal = 1;
                break;
            }
            if (back) {
                report_line(rs, "%s: skipped line %zu (byte %zu): seq %lld after %lld", name, i + 1, ls[i].off,
                            (long long)seq, (long long)rs->last);
                st->salvaged_records++;
                st->salvaged_bytes += (int64_t)ls[i].len + 1;
                damaged = 1;
                json_free(ch);
                continue;
            }
            report_line(rs, "%s: seq gap at line %zu (byte %zu): seq %lld after %lld (%lld missing)", name, i + 1,
                        ls[i].off, (long long)seq, (long long)rs->last, (long long)(seq - rs->last - 1));
            st->seq_gaps++;
            damaged = 1;
        }
        rs->past_snapshot = 1;
        rs->last = seq;
        replay_record(st, ch, seq);
    }
    if (!fatal && good_end < s.len) {
        size_t tail = s.len - good_end;
        if (!live) {
            /* journal.old was complete and fsynced before it was rotated: no tail can be torn */
            if (!rs->salvage) {
                snprintf(err, errlen,
                         "%s: %zu damaged byte(s) at offset %zu (a rotated journal is never torn); refusing to open. "
                         "Open with salvage (mx_store_opts.salvage or MODX_STORE_SALVAGE=1) to drop them",
                         name, tail, good_end);
                fatal = 1;
            } else {
                report_line(rs, "%s: skipped %zu damaged byte(s) at offset %zu", name, tail, good_end);
                st->salvaged_bytes += (int64_t)tail;
                damaged = 1;
            }
        } else {
            fprintf(stderr, "modx: journal: discarding %zu bytes of torn/corrupt tail at offset %zu\n", tail, good_end);
            if (rs->salvage) report_line(rs, "%s: cut %zu byte(s) of torn tail at offset %zu", name, tail, good_end);
        }
    }
    if (!fatal && damaged) {
        rs->damaged = 1;
        if (keep_damaged_copy(path, &s, rs) != 0) {
            snprintf(err, errlen, "salvage: cannot keep a copy of %s: %s", path, strerror(errno));
            fatal = 1;
        }
    }
    if (!fatal && live && good_end < s.len) {
        if (truncate(path, (off_t)good_end) != 0) {
            snprintf(err, errlen, "cannot truncate journal: %s", strerror(errno));
            fatal = 1;
        } else {
            st->recovered_truncated = 1;
            st->recovered_tail_bytes += (int64_t)(s.len - good_end);
        }
    }
    rc = fatal ? -1 : (int64_t)good_end;
    free(ls);
    sbuf_free(&s);
    return rc;
}

/* Salvage found damage: write the report, then a fresh snapshot of what was
 * recovered and an empty journal, so the next open is a clean one. */
static int salvage_finish(mx_store *st, replay_state *rs, const mx_store_opts *opts, char *err, size_t errlen) {
    char *path;
    if (opts && opts->report_path) path = xstrdup(opts->report_path);
    else {
        char name[64];
        snprintf(name, sizeof name, "recovery-%lld.txt", (long long)rs->stamp);
        path = path_join(st->dir, name);
    }
    sbuf head;
    sbuf_init(&head);
    sbuf_printf(&head,
                "modx store salvage report\ndir: %s\ntime (unix ms): %lld\nsnapshot seq: %lld\nrecovered seq: %lld\n"
                "skipped records / lines: %lld\nskipped bytes: %lld\nseq gaps accepted: %lld\n\n",
                st->dir, (long long)rs->stamp, (long long)rs->snap_seq, (long long)st->seq,
                (long long)st->salvaged_records, (long long)st->salvaged_bytes, (long long)st->seq_gaps);
    sbuf_put(&head, rs->report.data, rs->report.len);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int ok = fd >= 0 && write_all(fd, head.data, head.len) == 0 && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    sbuf_free(&head);
    if (!ok) {
        snprintf(err, errlen, "salvage: cannot write the report %s: %s", path, strerror(errno));
        free(path);
        return -1;
    }
    fprintf(stderr, "modx: salvage: recovered up to seq %lld, skipped %lld record(s); report: %s\n",
            (long long)st->seq, (long long)st->salvaged_records, path);
    st->recovery_report = path;
    if (mx_store_compact(st) != 0) {
        snprintf(err, errlen, "salvage: cannot write the recovered snapshot");
        return -1;
    }
    return 0;
}

mx_store *mx_store_open(const char *dir, const mx_coll_def *defs, int ncoll, size_t ring_cap,
                        const mx_store_hooks *hooks, char *err, size_t errlen) {
    return mx_store_open_ex(dir, defs, ncoll, ring_cap, hooks, NULL, err, errlen);
}

mx_store *mx_store_open_ex(const char *dir, const mx_coll_def *defs, int ncoll, size_t ring_cap,
                           const mx_store_hooks *hooks, const mx_store_opts *opts, char *err, size_t errlen) {
    int64_t t0 = mono_ms();
    if (mkdir_p(dir) != 0) {
        snprintf(err, errlen, "cannot create data dir %s: %s", dir, strerror(errno));
        return NULL;
    }
    mx_store *st = xcalloc(1, sizeof *st);
    st->dir = xstrdup(dir);
    st->journal_path = path_join(dir, "journal.log");
    st->snapshot_path = path_join(dir, "snapshot.json");
    st->journal_fd = -1;
    st->defs = defs;
    st->ncoll = ncoll;
    st->colls = xcalloc((size_t)(ncoll ? ncoll : 1), sizeof *st->colls);
    for (int i = 0; i < ncoll; i++) hmap_init(&st->colls[i]);
    for (int i = 0; i < ncoll; i++)
        for (const char *const *f = defs[i].index; f && *f; f++) st->nindexes++;
    st->indexes = xcalloc((size_t)(st->nindexes ? st->nindexes : 1), sizeof *st->indexes);
    for (int i = 0, k = 0; i < ncoll; i++)
        for (const char *const *f = defs[i].index; f && *f; f++) {
            st->indexes[k].coll = i;
            st->indexes[k].field = *f;
            k++;
        }
    st->ring_cap = ring_cap;
    st->ring = xcalloc(ring_cap ? ring_cap : 1, sizeof *st->ring);
    st->compact_threshold = 4 * 1024 * 1024;
    if (hooks) st->hooks = *hooks;
    st->journal_old_path = path_join(dir, "journal.old");

    replay_state rs;
    memset(&rs, 0, sizeof rs);
    sbuf_init(&rs.report);
    const char *env = getenv("MODX_STORE_SALVAGE");
    rs.salvage = (opts && opts->salvage) || (env && strcmp(env, "1") == 0);
    rs.stamp = now_ms();
    st->salvage = rs.salvage;
#define OPEN_FAIL() do { sbuf_free(&rs.report); mx_store_close(st); return NULL; } while (0)
    if (load_snapshot(st, &rs, err, errlen) != 0) OPEN_FAIL();
    rs.snap_seq = rs.last = st->seq;
    /* a background compaction was interrupted: its rotated journal holds the
     * records between the old snapshot and the current journal */
    int had_old = access(st->journal_old_path, F_OK) == 0;
    if (had_old && replay_file(st, st->journal_old_path, 0, &rs, err, errlen) < 0) OPEN_FAIL();
    int64_t good = replay_file(st, st->journal_path, 1, &rs, err, errlen);
    if (good < 0) OPEN_FAIL();
    st->journal_size = good;
    st->journal_fd = open(st->journal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (st->journal_fd < 0) {
        snprintf(err, errlen, "cannot open journal: %s", strerror(errno));
        OPEN_FAIL();
    }
    if (st->recovered_truncated) fsync(st->journal_fd);
    st->synced_seq = st->seq;
    if (rs.damaged) {
        if (salvage_finish(st, &rs, opts, err, errlen) != 0) OPEN_FAIL();
    } else if (had_old && mx_store_compact(st) != 0) {
        snprintf(err, errlen, "cannot merge %s into a new snapshot", st->journal_old_path);
        OPEN_FAIL();
    }
#undef OPEN_FAIL
    sbuf_free(&rs.report);
    st->open_ms = mono_ms() - t0;
    return st;
}

void mx_store_close(mx_store *st) {
    if (!st) return;
    if (st->compact_pid > 0) {
        int status;
        while (waitpid(st->compact_pid, &status, 0) < 0 && errno == EINTR) {}
        compact_done(st, status);
    }
    if (st->journal_fd >= 0) {
        if (!st->failed && flush_pending(st) == 0) fsync(st->journal_fd);
        close(st->journal_fd);
    }
    for (size_t i = 0; i < st->ring_len; i++) change_free(st, &st->ring[(st->ring_start + i) % st->ring_cap]);
    for (int i = 0; i < st->ncoll; i++) hmap_free(&st->colls[i]);
    for (int i = 0; i < st->nindexes; i++) ix_free(&st->indexes[i]);
    free(st->indexes);
    free(st->colls);
    free(st->ring);
    free(st->dir);
    free(st->journal_path);
    free(st->snapshot_path);
    free(st->journal_old_path);
    free(st->recovery_report);
    sbuf_free(&st->pending);
    free(st);
}

int mx_store_is_empty(const mx_store *st) {
    if (st->seq != 0) return 0;
    for (int i = 0; i < st->ncoll; i++)
        if (st->colls[i].count) return 0;
    return 1;
}

const json *mx_store_get(const mx_store *st, int coll, const char *key) {
    if (coll < 0 || coll >= st->ncoll || !key) return NULL;
    return hmap_get(&st->colls[coll], key);
}

size_t mx_store_count(const mx_store *st, int coll) { return st->colls[coll].count; }

const json **mx_store_all(const mx_store *st, int coll, size_t *n) {
    const hmap *m = &st->colls[coll];
    const json **out = xmalloc((m->count ? m->count : 1) * sizeof *out);
    size_t k = 0;
    for (size_t i = 0; i < m->cap; i++)
        if (m->slots[i].key) out[k++] = m->slots[i].val;
    *n = k;
    return out;
}

/* ---------------- mutations ---------------- */

/* key: storage key (computed from data for a put when NULL). Takes data. */
static int commit(mx_store *st, int coll, const char *op, const char *id, const char *key_in, json *data,
                  const char *client_id) {
    if (coll < 0 || coll >= st->ncoll) { json_free(data); return -1; }
    if (st->failed) {
        /* read-only after a journal I/O error: nothing more is accepted */
        json_free(data);
        st->rejected_writes++;
        st->rejected_pending = 1;
        errno = EROFS;
        return -1;
    }
    int64_t seq = st->seq + 1;
    int64_t ts = now_ms();
    char *key = key_in ? xstrdup(key_in) : key_for(st, coll, id, NULL, data);
    sbuf ch;
    sbuf_init(&ch);
    int had_extra = 0;
    write_change(st, &ch, seq, coll, op, id, key, data, client_id, ts, 0, &had_extra);

    /* group commit: records collect in memory; mx_store_sync writes them with
     * one write() and one fsync() */
    sbuf_printf(&st->pending, "%08x ", crc32_buf(ch.data, ch.len));
    sbuf_put(&st->pending, ch.data, ch.len);
    sbuf_putc(&st->pending, '\n');
    st->pending_records++;
    st->seq = seq;

    char *pub = NULL;
    unsigned fl = coll_flags(st, coll);
    if (!(fl & (MX_COLL_PRIVATE | MX_COLL_NO_RING))) {
        if ((fl & MX_COLL_REDACT) || had_extra) {
            sbuf p;
            sbuf_init(&p);
            write_change(st, &p, seq, coll, op, id, key, data, client_id, ts, 1, NULL);
            pub = sbuf_take(&p);
        } else {
            pub = sbuf_take(&ch);
        }
    }
    sbuf_free(&ch);
    record_change(st, seq, coll, op, id, key, data, client_id, ts, 0, pub);
    apply(st, coll, key, data);
    free(key);
    return 0;
}

int mx_store_put(mx_store *st, int coll, json *obj, const char *client_id) {
    const char *id = json_get_str(obj, "id");
    if (!id) { json_free(obj); return -1; }
    char *idc = xstrdup(id);
    int rc = commit(st, coll, "put", idc, NULL, obj, client_id);
    free(idc);
    return rc;
}

int mx_store_del_key(mx_store *st, int coll, const char *id, const char *key, const char *client_id) {
    char *idc = xstrdup(id);
    char *kc = xstrdup(key ? key : id);
    int rc = commit(st, coll, "del", idc, kc, NULL, client_id);
    free(idc);
    free(kc);
    return rc;
}

int mx_store_del(mx_store *st, int coll, const char *id, const char *client_id) {
    return mx_store_del_key(st, coll, id, NULL, client_id);
}

/* Failed: only a rejected mutation is "pending" (so the barrier answers the
 * request that made it); the failed batch never becomes durable. */
int mx_store_dirty(const mx_store *st) { return st->failed ? st->rejected_pending : st->synced_seq != st->seq; }

int mx_store_failed(const mx_store *st) { return st->failed; }

void mx_store_health_get(const mx_store *st, mx_store_health *h) {
    memset(h, 0, sizeof *h);
    h->failed = st->failed;
    h->fail_errno = st->fail_errno;
    h->fail_op = st->fail_op;
    h->seq = st->seq;
    h->synced_seq = st->synced_seq;
    h->pending_records = st->pending_records;
    h->rejected_writes = st->rejected_writes;
    h->journal_size = st->journal_size;
}

/* A journal write or fsync failed. After a failed fsync the kernel may have
 * dropped the dirty pages and cleared the error, so a later fsync "succeeding"
 * proves nothing (fsyncgate); after a failed write the records are gone. Either
 * way the changes of the batch, and everything after them, can never be
 * reported durable: the store becomes read-only (sticky) until it is reopened,
 * which replays exactly what reached the disk. */
static void store_fail(mx_store *st, const char *op, int err) {
    if (st->failed) return;
    st->failed = 1;
    st->fail_op = op;
    st->fail_errno = err;
    st->pending.len = 0;
    st->pending_records = 0;
    fprintf(stderr, "modx: store is read-only after a journal %s failure (%s); reopen to recover\n", op,
            err ? strerror(err) : "injected fault");
}

/* Write the pending records (one write). On failure the partial tail is cut
 * so the file stays parseable, and the store fails (read-only). */
static int flush_pending(mx_store *st) {
    if (st->failed) return -1;
    if (!st->pending.len) return 0;
    if (write_all(st->journal_fd, st->pending.data, st->pending.len) != 0) {
        int e = errno;
        if (ftruncate(st->journal_fd, (off_t)st->journal_size) != 0) { /* recovery cuts it */ }
        store_fail(st, "write", e);
        errno = e;
        return -1;
    }
    st->journal_size += (int64_t)st->pending.len;
    st->writes++;
    st->pending.len = 0;
    st->pending_records = 0;
    if (st->pending.cap > (1u << 20)) sbuf_free(&st->pending);
    return 0;
}

int mx_store_sync(mx_store *st) {
    if (st->failed) {
        st->rejected_pending = 0;
        errno = EROFS;
        return MX_STORE_EFAILED;
    }
    if (st->synced_seq == st->seq) return 0;
    int64_t batch = st->seq - st->synced_seq;
    if (st->fault_syncs > 0) {
        st->fault_syncs--;
        store_fail(st, "fsync", 0);
        return -1;
    }
    if (flush_pending(st) != 0) return -1;
    if (fsync(st->journal_fd) != 0) {
        store_fail(st, "fsync", errno); /* never retried */
        return -1;
    }
    st->fsyncs++;
    if (batch > st->max_batch) st->max_batch = batch;
    int64_t from = st->synced_seq;
    st->synced_seq = st->seq;
    if (st->listener) {
        for (size_t i = 0; i < st->ring_len; i++) {
            const mx_change *e = mx_store_ring_at(st, i);
            if (e->seq > from && e->json) st->listener(st->listener_ctx, e);
        }
    }
    if (st->journal_size > st->compact_threshold && st->compact_pid <= 0) {
        int rc = st->sync_compaction ? mx_store_compact(st) : mx_store_compact_bg(st);
        if (rc < 0) fprintf(stderr, "modx: compaction failed (journal kept)\n");
    }
    return 0;
}

int mx_store_pending_cb(void *st) { return mx_store_dirty(st); }
int mx_store_commit_cb(void *st) { return mx_store_sync(st); }

void mx_store_tick(mx_store *st) {
    if (st->compact_pid <= 0) return;
    int status;
    pid_t r = waitpid(st->compact_pid, &status, WNOHANG);
    if (r == st->compact_pid) compact_done(st, status);
    else if (r < 0 && errno != EINTR) { st->compact_pid = 0; st->compact_failures++; }
}

/* ---------------- snapshot ---------------- */

static int cmp_row_id(const void *a, const void *b) {
    const json *x = *(const json *const *)a, *y = *(const json *const *)b;
    const char *ix = json_get_str(x, "id"), *iy = json_get_str(y, "id");
    return strcmp(ix ? ix : "", iy ? iy : ""); /* rows always have one; never crash a snapshot over it */
}

static void serialize_snapshot(const mx_store *st, sbuf *out) {
    size_t base = out->len;
    sbuf_puts(out, SNAP_CRC_PREFIX "00000000/000000000000\",");
    sbuf_printf(out, "\"seq\":%lld,\"collections\":{", (long long)st->seq);
    for (int c = 0; c < st->ncoll; c++) {
        if (c) sbuf_putc(out, ',');
        json_write_string(out, st->defs[c].name, strlen(st->defs[c].name));
        sbuf_puts(out, ":[");
        size_t n;
        const json **all = mx_store_all(st, c, &n);
        qsort(all, n, sizeof *all, cmp_row_id); /* deterministic output */
        for (size_t i = 0; i < n; i++) {
            if (i) sbuf_putc(out, ',');
            sbuf_putc(out, '\n');
            json_write(out, all[i]);
        }
        free(all);
        sbuf_putc(out, ']');
    }
    sbuf_puts(out, "}}\n");
    /* the checksum of everything after the prefix goes into the prefix */
    char hdr[24];
    size_t body = out->len - base - SNAP_CRC_HDR;
    snprintf(hdr, sizeof hdr, "%08x/%012llu", crc32_buf(out->data + base + SNAP_CRC_HDR, body), (unsigned long long)body);
    memcpy(out->data + base + 10, hdr, 21);
}

/* Writes buf as the snapshot: tmp file, fsync, rename, fsync dir. */
static int write_snapshot_file(const mx_store *st, const sbuf *out, int crash_test) {
    char *tmp = path_join(st->dir, "snapshot.json.tmp");
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int rc = -1;
    if (fd >= 0) {
        if (crash_test) {
            /* tests: die halfway through the file, like a power cut would */
            if (write_all(fd, out->data, out->len / 2) == 0) fsync(fd);
            raise(SIGKILL);
        }
        if (write_all(fd, out->data, out->len) == 0 && fsync(fd) == 0) rc = 0;
        close(fd);
    }
    if (rc == 0 && rename(tmp, st->snapshot_path) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
    else fsync_dir(st->dir);
    free(tmp);
    return rc;
}

static void compact_done(mx_store *st, int status) {
    st->compact_pid = 0;
    st->last_compact_ms = mono_ms() - st->compact_started;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        /* the new snapshot covers the rotated journal */
        if (unlink(st->journal_old_path) == 0) fsync_dir(st->dir);
        st->compactions++;
    } else {
        /* journal.old stays: the next compaction is a synchronous one that covers it */
        fprintf(stderr, "modx: background snapshot failed (status %d); journal kept\n", status);
        st->compact_failures++;
    }
}

int mx_store_compact_bg(mx_store *st) {
    if (st->compact_pid > 0) return 1;
    /* a failed store must not write its memory (which holds the lost batch) as a snapshot */
    if (st->failed) return -1;
    /* a previous background run failed: its rotated journal must be covered first */
    if (access(st->journal_old_path, F_OK) == 0) return mx_store_compact(st);
    int64_t t0 = mono_ms();
    if (flush_pending(st) != 0) return -1;
    if (fsync(st->journal_fd) != 0) { store_fail(st, "fsync", errno); return -1; }
    if (st->hooks.before_snapshot) st->hooks.before_snapshot(st->hooks.ctx, st);
    /* rotate: journal.log -> journal.old (records <= seq), fresh journal.log for what follows */
    if (rename(st->journal_path, st->journal_old_path) != 0) return -1;
    int nfd = open(st->journal_path, O_WRONLY | O_CREAT | O_APPEND | O_TRUNC, 0644);
    if (nfd < 0) {
        int e = errno;
        if (rename(st->journal_old_path, st->journal_path) != 0) { /* still readable as journal.old */ }
        errno = e;
        return -1;
    }
    fsync_dir(st->dir);
    close(st->journal_fd);
    st->journal_fd = nfd;
    st->journal_size = 0;
    pid_t pid = fork();
    if (pid < 0) {
        /* no child: do it here; the rotated journal is covered by the snapshot */
        return mx_store_compact(st);
    }
    if (pid == 0) {
        /* child: memory is a copy-on-write image of the store at seq */
        sbuf out;
        sbuf_init(&out);
        serialize_snapshot(st, &out);
        _exit(write_snapshot_file(st, &out, st->test_crash_compaction) == 0 ? 0 : 1);
    }
    st->compact_pid = pid;
    st->compact_started = t0;
    st->last_compact_block_ms = mono_ms() - t0;
    return 0;
}

int mx_store_compact(mx_store *st) {
    int64_t t0 = mono_ms();
    if (st->compact_pid > 0) {
        int status;
        while (waitpid(st->compact_pid, &status, 0) < 0 && errno == EINTR) {}
        compact_done(st, status);
    }
    if (st->failed) return -1; /* its memory holds changes that never reached the disk */
    /* records not written yet would be lost by the truncate below: they are in
     * memory (and so in the snapshot), but write them anyway so a failed
     * snapshot leaves the journal complete */
    if (flush_pending(st) != 0) return -1;
    if (st->hooks.before_snapshot) st->hooks.before_snapshot(st->hooks.ctx, st);
    sbuf out;
    sbuf_init(&out);
    serialize_snapshot(st, &out);
    int rc = write_snapshot_file(st, &out, 0);
    sbuf_free(&out);
    if (rc != 0) {
        fprintf(stderr, "modx: snapshot write failed: %s\n", strerror(errno));
        return -1;
    }
    /* the snapshot now covers every journal record; drop them */
    if (unlink(st->journal_old_path) == 0) fsync_dir(st->dir);
    if (ftruncate(st->journal_fd, 0) != 0 || fsync(st->journal_fd) != 0) {
        fprintf(stderr, "modx: journal truncate failed: %s\n", strerror(errno));
        return -1; /* harmless: records <= snapshot seq are skipped on replay */
    }
    st->journal_size = 0;
    st->last_compact_ms = st->last_compact_block_ms = mono_ms() - t0;
    st->compactions++;
    return 0;
}

/* ---------------- catch-up ---------------- */

int mx_store_for_each_since(const mx_store *st, int64_t since, mx_change_filter keep, void *keep_ctx,
                            mx_change_fn fn, void *ctx) {
    if (since > st->seq) return -1; /* client is ahead of us: data dir was reset */
    if (since < st->seq) {
        int64_t oldest = st->ring_len ? mx_store_ring_at(st, 0)->seq : st->seq + 1;
        if (since + 1 < oldest) return -1;
    }
    for (size_t i = 0; i < st->ring_len; i++) {
        const mx_change *e = mx_store_ring_at(st, i);
        if (e->seq <= since || e->seq > st->synced_seq || !e->json) continue;
        if (keep && !keep(keep_ctx, e)) continue;
        fn(ctx, e);
    }
    return 0;
}
