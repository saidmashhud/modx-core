#ifndef MODX_STORE_H
#define MODX_STORE_H

/* modx store: an in-memory document store made durable by a snapshot file and
 * an append-only journal.
 *
 *   <dir>/snapshot.json   {"seq":N,"collections":{"<name>":[row,...],...}}
 *   <dir>/journal.log     one record per line: "<crc32 hex8> <change json>\n"
 *
 * Rows are JSON objects with a string "id". Every put/del gets the next seq,
 * is appended to the journal and applied in memory. mx_store_sync() makes
 * everything appended so far durable (fsync) and only then hands the changes
 * to the listener (SSE fan-out etc.). On open the snapshot is loaded and the
 * journal replayed; a torn or corrupt tail (bad CRC, no newline, bad JSON) is
 * cut off. When the journal grows past compact_threshold a new snapshot is
 * written and the journal truncated.
 *
 * Recent public changes are kept in a ring (catch-up for clients that were
 * offline: mx_store_for_each_since). What a change means for visibility is
 * up to the application: the scope hooks attach an opaque value to every ring
 * entry when it is committed, and the application filters on it.
 *
 * Collections are registered by the application (mx_coll_def). */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "modx/json.h"
#include "modx/util.h"

/* ---- hash map: string key -> owned JSON value (open addressing) ---- */
typedef struct {
    char *key;
    json *val;
} hentry;

typedef struct {
    hentry *slots;
    size_t cap;
    size_t count;
} hmap;

void hmap_init(hmap *m);
void hmap_free(hmap *m);
json *hmap_get(const hmap *m, const char *key);
/* Takes ownership of val; frees a value it replaces. */
void hmap_put(hmap *m, const char *key, json *val);
/* Like hmap_put, but returns the replaced value (caller frees) instead of freeing it. */
json *hmap_swap(hmap *m, const char *key, json *val);
/* 1 if a key was removed (its value freed). */
int hmap_del(hmap *m, const char *key);
/* Removes key and returns its value without freeing it (NULL if absent). */
json *hmap_take(hmap *m, const char *key);

/* ---- collections ---- */
enum {
    MX_COLL_PRIVATE = 1, /* persisted, never handed out as a public change (sessions, secrets) */
    MX_COLL_NO_RING = 2, /* takes no slot in the change ring (high-volume internal rows) */
    MX_COLL_REDACT = 4   /* public change JSON is built with hooks.write_public (hidden fields) */
};

typedef struct {
    const char *name;
    unsigned flags;
    /* Fields to index (NULL-terminated, may be NULL): mx_store_find looks
     * rows up by the string value of such a field without a full scan. */
    const char *const *index;
} mx_coll_def;

/* Secondary index of one field: value -> set of storage keys. */
typedef struct mx_ixbucket mx_ixbucket;
typedef struct {
    int coll;
    const char *field;
    mx_ixbucket **slots;
    size_t cap, count;
} mx_index;

/* One retained change. json is the public serialized change
 * ({"seq","entity","op","id","data","client_id"?,"ts"}), NULL for private
 * collections. scope is whatever hooks.scope_new returned. */
typedef struct {
    int64_t seq;
    int coll;
    char *json;
    void *scope;
} mx_change;

typedef struct mx_store mx_store;

/* Facts about one change, handed to scope_new before it is applied. */
typedef struct {
    int64_t seq;
    int coll;
    const char *op;        /* "put" / "del" */
    const char *id;
    const char *key;       /* storage key */
    const json *old;       /* current row (NULL = none) */
    const json *row;       /* new row (put) or NULL (del) */
    const char *client_id; /* may be NULL */
    int64_t ts;
    int replay;            /* 1 while the journal is replayed on open */
    const char *json;      /* the public change JSON (NULL for private collections) */
} mx_commit_info;

typedef struct {
    void *ctx;
    /* Storage key of a row (NULL = the row's id). rec is the journal record
     * while replaying (NULL otherwise), row the row being stored (NULL for a
     * del). Returned string is freed by the store. */
    char *(*row_key)(void *ctx, int coll, const char *id, const json *rec, const json *row);
    /* Append extra journal fields (",\"k\":v") for a record; they are kept in
     * the journal only, never in the public change. */
    void (*journal_extra)(void *ctx, int coll, const char *id, const char *key, const json *row, sbuf *out);
    /* Serialize a row for clients (MX_COLL_REDACT collections). NULL = json_write. */
    void (*write_public)(void *ctx, int coll, const json *row, sbuf *out);
    /* Opaque scope for a ring entry; freed with scope_free. */
    void *(*scope_new)(void *ctx, mx_store *st, const mx_commit_info *ci);
    void (*scope_free)(void *ctx, void *scope);
    /* After a row was stored / removed (live and replay). old is still valid. */
    void (*on_apply)(void *ctx, mx_store *st, int coll, const char *key, const json *old, const json *row);
    /* Right before a snapshot is serialized (e.g. prune tombstones). */
    void (*before_snapshot)(void *ctx, mx_store *st);
} mx_store_hooks;

/* Called after a change became durable, in seq order, for every public change. */
typedef void (*mx_store_listener)(void *ctx, const mx_change *ch);

struct mx_store {
    char *dir;
    char *journal_path;
    char *snapshot_path;
    int journal_fd;
    int64_t journal_size;  /* bytes on disk */
    int64_t seq;           /* last assigned seq */
    int64_t synced_seq;    /* last seq known durable and broadcast */
    int ncoll;
    const mx_coll_def *defs;
    hmap *colls;           /* ncoll maps */
    mx_index *indexes;     /* from the collection definitions */
    int nindexes;
    mx_change *ring;
    size_t ring_cap, ring_start, ring_len;
    mx_store_listener listener;
    void *listener_ctx;
    mx_store_hooks hooks;
    int64_t compact_threshold; /* bytes; journal is compacted when it grows past this */
    int recovered_truncated;   /* set when a torn/corrupt tail was cut on open */
    void *app;                 /* free for the application */
    int loading;               /* 1 while on_apply sees the rows of the snapshot being loaded */
    /* stats */
    int64_t open_ms;           /* time db_open took (load + replay) */
    int64_t last_compact_ms;   /* duration of the last compaction (parent-side) */
    int64_t fsyncs;            /* number of journal fsyncs */
    int64_t writes;            /* number of journal write batches */
    int64_t max_batch;         /* most changes made durable by one fsync */
    char *journal_old_path;    /* journal rotated away by a running background compaction */
    pid_t compact_pid;         /* background compaction child (0 = none) */
    int64_t compact_started;
    int sync_compaction;       /* 1 = compact in-process (blocks the loop), 0 = fork (default) */
    int test_crash_compaction; /* testing: the compaction child dies halfway through the snapshot */
    int64_t compactions, compact_failures;
    int64_t last_compact_block_ms; /* how long the last compaction blocked the caller */
    int fault_syncs;           /* testing: the next N syncs fail like an fsync error would */
    sbuf pending;              /* group commit: records not written yet */
    int64_t pending_records;
};

/* Opens (creating if needed) the data dir, loads the snapshot, replays the
 * journal, truncates a torn/corrupt tail. defs must outlive the store.
 * Returns NULL and fills err on fatal errors. */
mx_store *mx_store_open(const char *dir, const mx_coll_def *defs, int ncoll, size_t ring_cap,
                        const mx_store_hooks *hooks, char *err, size_t errlen);
void mx_store_close(mx_store *st);

int mx_store_coll(const mx_store *st, const char *name);
const char *mx_store_coll_name(const mx_store *st, int coll);
int mx_store_is_empty(const mx_store *st);

/* Lookups by storage key return borrowed pointers. */
const json *mx_store_get(const mx_store *st, int coll, const char *key);
size_t mx_store_count(const mx_store *st, int coll);
/* Malloc'ed array of borrowed pointers (caller frees the array only). */
const json **mx_store_all(const mx_store *st, int coll, size_t *n);

/* Rows of coll whose string field equals value (borrowed pointers in a
 * malloc'ed array; caller frees the array). Uses the index when the field is
 * declared in mx_coll_def.index, else scans. Order is unspecified. */
const json **mx_store_find(const mx_store *st, int coll, const char *field, const char *value, size_t *n);
/* Number of rows mx_store_find would return (O(1) for indexed fields). */
size_t mx_store_find_count(const mx_store *st, int coll, const char *field, const char *value);
/* 1 when (coll, field) is indexed. */
int mx_store_indexed(const mx_store *st, int coll, const char *field);

/* Mutations: journal, apply in memory, record in the ring (not durable until
 * mx_store_sync). put takes ownership of obj (which needs a string "id"); the
 * key comes from hooks.row_key. del_key removes the row stored under key
 * (id is what the change reports). Return 0, or -1 on I/O error. */
int mx_store_put(mx_store *st, int coll, json *obj, const char *client_id);
int mx_store_del(mx_store *st, int coll, const char *id, const char *client_id);
int mx_store_del_key(mx_store *st, int coll, const char *id, const char *key, const char *client_id);

/* Group commit: mutations only queue their journal records. mx_store_sync
 * writes everything queued with one write(), fsyncs once and then notifies
 * the listener of every newly durable change. May start a compaction.
 * Returns 0, or -1 on I/O error (nothing of the batch may be reported as
 * durable then). */
int mx_store_sync(mx_store *st);
/* Adapters for http_server's durability barrier (ctx = the store). */
int mx_store_pending_cb(void *st);
int mx_store_commit_cb(void *st);
/* 1 when changes were committed that are not durable yet. */
int mx_store_dirty(const mx_store *st);

/* Write the snapshot (tmp + fsync + rename) and truncate the journal, in
 * this process (blocks until done). Waits for a running background one first. */
int mx_store_compact(mx_store *st);
/* Background compaction: rotate journal.log to journal.old (fresh journal
 * for new records), fork, and let the child write the snapshot from its
 * copy-on-write image; mx_store_tick reaps it and drops journal.old. The
 * parent is blocked only for the rotation and fork(). A crash anywhere in
 * between loses nothing: open replays snapshot + journal.old + journal.log
 * and merges them. Returns 0 (started), 1 (one is already running), -1. */
int mx_store_compact_bg(mx_store *st);

/* Calls fn for every durable retained public change with seq > since that
 * keep(scope) accepts (keep NULL = all). Returns 0, or -1 when since is
 * outside the retained window (client must re-bootstrap). */
typedef int (*mx_change_filter)(void *ctx, const mx_change *ch);
typedef void (*mx_change_fn)(void *ctx, const mx_change *ch);
int mx_store_for_each_since(const mx_store *st, int64_t since, mx_change_filter keep, void *keep_ctx,
                            mx_change_fn fn, void *ctx);
/* i-th retained change, oldest first (i < ring_len). */
const mx_change *mx_store_ring_at(const mx_store *st, size_t i);

/* Event loop housekeeping (reaps a background compaction). Cheap; call it
 * from the server tick. */
void mx_store_tick(mx_store *st);

#endif
