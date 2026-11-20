#include "modx/store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static int load_snapshot(mx_store *st, char *err, size_t errlen) {
    sbuf s;
    sbuf_init(&s);
    if (read_file(st->snapshot_path, &s) != 0) {
        sbuf_free(&s);
        if (errno == ENOENT) return 0;
        snprintf(err, errlen, "cannot read %s: %s", st->snapshot_path, strerror(errno));
        return -1;
    }
    if (s.len == 0) { sbuf_free(&s); return 0; }
    char perr[128];
    json *root = json_parse(s.data, s.len, perr, sizeof perr);
    sbuf_free(&s);
    if (!root || root->type != JSON_OBJECT) {
        snprintf(err, errlen, "corrupt snapshot %s: %s", st->snapshot_path, root ? "not an object" : perr);
        json_free(root);
        return -1;
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
                hmap_put(&st->colls[c], key, obj);
                free(key);
            }
        }
    }
    json_free(root);
    if (st->hooks.on_apply) {
        /* derived state (indexes) sees every loaded row once */
        for (int c = 0; c < st->ncoll; c++) {
            hmap *m = &st->colls[c];
            for (size_t i = 0; i < m->cap; i++)
                if (m->slots[i].key) st->hooks.on_apply(st->hooks.ctx, st, c, m->slots[i].key, NULL, m->slots[i].val);
        }
    }
    return 0;
}

/* Parses one "<crc8> <json>" line. Returns the record or NULL (corrupt). */
static json *parse_record(const char *line, size_t line_len) {
    if (line_len < 10 || line[8] != ' ') return NULL;
    uint32_t want = 0;
    for (int i = 0; i < 8; i++) {
        char c = line[i];
        int h = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (h < 0) return NULL;
        want = (want << 4) | (uint32_t)h;
    }
    const char *body = line + 9;
    size_t body_len = line_len - 9;
    if (crc32_buf(body, body_len) != want) return NULL;
    json *ch = json_parse(body, body_len, NULL, 0);
    if (!ch || ch->type != JSON_OBJECT || !json_is_int(json_get(ch, "seq"))) { json_free(ch); return NULL; }
    return ch;
}

/* Replays one journal file. truncate_tail: cut a torn/corrupt tail (the live
 * journal); otherwise stop at it. Returns bytes of good records, or -1. */
static int64_t replay_file(mx_store *st, const char *path, int64_t snap_seq, int truncate_tail, char *err,
                           size_t errlen) {
    sbuf s;
    sbuf_init(&s);
    if (read_file(path, &s) != 0) {
        sbuf_free(&s);
        if (errno == ENOENT) return 0;
        snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    size_t pos = 0, good_end = 0;
    while (pos < s.len) {
        char *nl = memchr(s.data + pos, '\n', s.len - pos);
        if (!nl) break; /* torn tail: no terminating newline */
        size_t line_len = (size_t)(nl - (s.data + pos));
        json *ch = parse_record(s.data + pos, line_len);
        if (!ch) break;
        int64_t seq = (int64_t)json_get(ch, "seq")->number;
        if (seq > snap_seq && seq > st->seq) {
            int coll = mx_store_coll(st, json_get_str(ch, "entity"));
            const char *op = json_get_str(ch, "op"), *cid = json_get_str(ch, "id");
            json *data = json_get(ch, "data");
            if (data && data->type != JSON_OBJECT) data = NULL;
            int is_put = op && strcmp(op, "put") == 0, is_del = op && strcmp(op, "del") == 0;
            if (coll < 0 || !cid || (!is_put && !is_del) || (is_put && !data)) { json_free(ch); break; }
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
        }
        json_free(ch);
        pos += line_len + 1;
        good_end = pos;
    }
    if (good_end < s.len && truncate_tail) {
        fprintf(stderr, "modx: journal: discarding %zu bytes of torn/corrupt tail at offset %zu\n",
                s.len - good_end, good_end);
        if (truncate(path, (off_t)good_end) != 0) {
            snprintf(err, errlen, "cannot truncate journal: %s", strerror(errno));
            sbuf_free(&s);
            return -1;
        }
        st->recovered_truncated = 1;
    }
    sbuf_free(&s);
    return (int64_t)good_end;
}

mx_store *mx_store_open(const char *dir, const mx_coll_def *defs, int ncoll, size_t ring_cap,
                        const mx_store_hooks *hooks, char *err, size_t errlen) {
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
    st->ring_cap = ring_cap;
    st->ring = xcalloc(ring_cap ? ring_cap : 1, sizeof *st->ring);
    st->compact_threshold = 4 * 1024 * 1024;
    if (hooks) st->hooks = *hooks;

    if (load_snapshot(st, err, errlen) != 0) { mx_store_close(st); return NULL; }
    int64_t snap_seq = st->seq;
    int64_t good = replay_file(st, st->journal_path, snap_seq, 1, err, errlen);
    if (good < 0) { mx_store_close(st); return NULL; }
    st->journal_size = good;
    st->journal_fd = open(st->journal_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (st->journal_fd < 0) {
        snprintf(err, errlen, "cannot open journal: %s", strerror(errno));
        mx_store_close(st);
        return NULL;
    }
    if (st->recovered_truncated) fsync(st->journal_fd);
    st->synced_seq = st->seq;
    st->open_ms = mono_ms() - t0;
    return st;
}

void mx_store_close(mx_store *st) {
    if (!st) return;
    if (st->journal_fd >= 0) {
        fsync(st->journal_fd);
        close(st->journal_fd);
    }
    for (size_t i = 0; i < st->ring_len; i++) change_free(st, &st->ring[(st->ring_start + i) % st->ring_cap]);
    for (int i = 0; i < st->ncoll; i++) hmap_free(&st->colls[i]);
    free(st->colls);
    free(st->ring);
    free(st->dir);
    free(st->journal_path);
    free(st->snapshot_path);
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
    int64_t seq = st->seq + 1;
    int64_t ts = now_ms();
    char *key = key_in ? xstrdup(key_in) : key_for(st, coll, id, NULL, data);
    sbuf ch;
    sbuf_init(&ch);
    int had_extra = 0;
    write_change(st, &ch, seq, coll, op, id, key, data, client_id, ts, 0, &had_extra);

    sbuf line;
    sbuf_init(&line);
    sbuf_printf(&line, "%08x ", crc32_buf(ch.data, ch.len));
    sbuf_put(&line, ch.data, ch.len);
    sbuf_putc(&line, '\n');
    if (write_all(st->journal_fd, line.data, line.len) != 0) {
        fprintf(stderr, "modx: journal write failed: %s\n", strerror(errno));
        /* drop a partially written record so later appends stay parseable */
        if (ftruncate(st->journal_fd, (off_t)st->journal_size) != 0) { /* recovery handles it */ }
        sbuf_free(&line);
        sbuf_free(&ch);
        json_free(data);
        free(key);
        return -1;
    }
    st->journal_size += (int64_t)line.len;
    sbuf_free(&line);
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

int mx_store_dirty(const mx_store *st) { return st->synced_seq != st->seq; }

int mx_store_sync(mx_store *st) {
    if (st->synced_seq == st->seq) return 0;
    if (fsync(st->journal_fd) != 0) {
        fprintf(stderr, "modx: journal fsync failed: %s\n", strerror(errno));
        return -1;
    }
    st->fsyncs++;
    int64_t from = st->synced_seq;
    st->synced_seq = st->seq;
    if (st->listener) {
        for (size_t i = 0; i < st->ring_len; i++) {
            const mx_change *e = mx_store_ring_at(st, i);
            if (e->seq > from && e->json) st->listener(st->listener_ctx, e);
        }
    }
    if (st->journal_size > st->compact_threshold) {
        if (mx_store_compact(st) != 0) fprintf(stderr, "modx: compaction failed (journal kept)\n");
    }
    return 0;
}

void mx_store_tick(mx_store *st) { (void)st; }

/* ---------------- snapshot ---------------- */

static int cmp_row_id(const void *a, const void *b) {
    const json *x = *(const json *const *)a, *y = *(const json *const *)b;
    return strcmp(json_get_str(x, "id"), json_get_str(y, "id"));
}

static void serialize_snapshot(const mx_store *st, sbuf *out) {
    sbuf_printf(out, "{\"seq\":%lld,\"collections\":{", (long long)st->seq);
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
}

int mx_store_compact(mx_store *st) {
    int64_t t0 = mono_ms();
    if (st->hooks.before_snapshot) st->hooks.before_snapshot(st->hooks.ctx, st);
    sbuf out;
    sbuf_init(&out);
    serialize_snapshot(st, &out);
    char *tmp = path_join(st->dir, "snapshot.json.tmp");
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int rc = -1;
    if (fd >= 0) {
        if (write_all(fd, out.data, out.len) == 0 && fsync(fd) == 0) rc = 0;
        close(fd);
    }
    sbuf_free(&out);
    if (rc == 0 && rename(tmp, st->snapshot_path) != 0) rc = -1;
    if (rc != 0) {
        fprintf(stderr, "modx: snapshot write failed: %s\n", strerror(errno));
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    fsync_dir(st->dir);
    /* the snapshot now covers every journal record; drop them */
    if (ftruncate(st->journal_fd, 0) != 0 || fsync(st->journal_fd) != 0) {
        fprintf(stderr, "modx: journal truncate failed: %s\n", strerror(errno));
        return -1; /* harmless: records <= snapshot seq are skipped on replay */
    }
    st->journal_size = 0;
    st->last_compact_ms = mono_ms() - t0;
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
