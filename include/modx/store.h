#ifndef MODX_STORE_H
#define MODX_STORE_H

/** @file store.h
 * @brief In-memory document store made durable by a snapshot file and an
 * append-only journal.
 *
 * @verbatim
   <dir>/snapshot.json   {"crc32":"<hex8>/<len12>","seq":N,"collections":{"<name>":[row,...],...}}
   <dir>/journal.log     one record per line: "<crc32 hex8> <change json>\n"
   @endverbatim
 *
 * Rows are JSON objects with a string "id". Every put/del gets the next seq,
 * is appended to the journal and applied in memory. mx_store_sync() makes
 * everything appended so far durable (fsync) and only then hands the changes
 * to the listener (SSE fan-out etc.). On open the snapshot is loaded and the
 * journal replayed; only a torn tail (the bytes after the last valid record)
 * is cut off. Damage anywhere else, or a seq gap, makes the open fail unless
 * salvage is asked for (mx_store_opts). When the journal grows past
 * compact_threshold a new snapshot is written and the journal truncated.
 *
 * The snapshot header `"crc32":"<hex8>/<len12>"` holds the CRC-32 and the
 * length of the rest of the file. A snapshot whose length differs from the
 * recorded one (rewritten by another tool) is loaded without the check, with
 * a warning (mx_store::snapshot_unchecked).
 *
 * Threads: a store belongs to one thread (the event loop). Background
 * compaction forks; fork only from that thread, and never touch the store
 * from worker threads (mx_pool jobs).
 *
 * Recent public changes are kept in a ring (catch-up for clients that were
 * offline: mx_store_for_each_since()). What a change means for visibility is
 * up to the application: the scope hooks attach an opaque value to every ring
 * entry when it is committed, and the application filters on it.
 *
 * Collections are registered by the application (mx_coll_def).
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "modx/json.h"
#include "modx/util.h"

/* ---- hash map: string key -> owned JSON value (open addressing) ---- */

/** One slot of an hmap (key NULL = empty). */
typedef struct {
    char *key; /**< owned copy of the key, NULL for an empty slot */
    json *val; /**< owned value */
} hentry;

/** Hash map from string keys to owned JSON values (open addressing, linear
 * probing). Zero-initialized or hmap_init() = empty. */
typedef struct {
    hentry *slots; /**< cap slots (NULL while empty) */
    size_t cap;    /**< slot count (0 or a power of two) */
    size_t count;  /**< keys stored */
} hmap;

/** Makes @p m empty without freeing anything. */
void hmap_init(hmap *m);
/** Frees every key and value and the slots; @p m is empty afterwards. */
void hmap_free(hmap *m);
/** Value stored under @p key (borrowed), or NULL. */
json *hmap_get(const hmap *m, const char *key);
/** Stores @p val under @p key (the key is copied). Takes ownership of val;
 * frees a value it replaces. */
void hmap_put(hmap *m, const char *key, json *val);
/** Like hmap_put(), but returns the replaced value (caller frees) instead of
 * freeing it; NULL when the key was new. */
json *hmap_swap(hmap *m, const char *key, json *val);
/** Removes @p key and frees its value. @return 1 if a key was removed, 0 if
 * absent. */
int hmap_del(hmap *m, const char *key);
/** Removes @p key and returns its value without freeing it (caller owns it;
 * NULL if absent). */
json *hmap_take(hmap *m, const char *key);

/* ---- collections ---- */

/** Collection flags (mx_coll_def::flags). */
enum {
    MX_COLL_PRIVATE = 1, /**< persisted, never handed out as a public change (sessions, secrets) */
    MX_COLL_NO_RING = 2, /**< takes no slot in the change ring (high-volume internal rows) */
    MX_COLL_REDACT = 4   /**< public change JSON is built with mx_store_hooks::write_public (hidden fields) */
};

/** Definition of one collection. The array passed to mx_store_open() must
 * outlive the store; a collection's number is its index in that array. */
typedef struct {
    const char *name; /**< name in the snapshot and the journal */
    unsigned flags;   /**< MX_COLL_* bits */
    /** Fields to index (NULL-terminated, may be NULL): mx_store_find() looks
     * rows up by the string value of such a field without a full scan. */
    const char *const *index;
} mx_coll_def;

/** Opaque bucket of an mx_index. */
typedef struct mx_ixbucket mx_ixbucket;
/** Secondary index of one field: value -> set of storage keys. Internal;
 * use mx_store_find(). */
typedef struct {
    int coll;             /**< collection number */
    const char *field;    /**< indexed field (borrowed from mx_coll_def::index) */
    mx_ixbucket **slots;  /**< buckets, one per distinct value */
    size_t cap, count;    /**< slot count, buckets in use */
} mx_index;

/** One retained change. */
typedef struct {
    int64_t seq;  /**< seq of the change */
    int coll;     /**< collection number */
    /** The public serialized change
     * (`{"seq","entity","op","id","data","client_id"?,"ts"}`), NULL for
     * private collections. Owned by the store. */
    char *json;
    void *scope;  /**< whatever mx_store_hooks::scope_new returned (freed with scope_free) */
} mx_change;

/** A store (see struct mx_store). */
typedef struct mx_store mx_store;

/** Facts about one change, handed to mx_store_hooks::scope_new before it is
 * applied. All pointers are borrowed for the duration of the call. */
typedef struct {
    int64_t seq;           /**< seq of the change */
    int coll;              /**< collection number */
    const char *op;        /**< "put" / "del" */
    const char *id;        /**< row id */
    const char *key;       /**< storage key */
    const json *old;       /**< current row (NULL = none) */
    const json *row;       /**< new row (put) or NULL (del) */
    const char *client_id; /**< may be NULL */
    int64_t ts;            /**< commit time (Unix ms) */
    int replay;            /**< 1 while the journal is replayed on open */
    const char *json;      /**< the public change JSON (NULL for private collections) */
} mx_commit_info;

/** Application hooks. Every member may be NULL; ctx is passed to each. */
typedef struct {
    void *ctx; /**< passed as the first argument of every hook */
    /** Storage key of a row (NULL = the row's id). rec is the journal record
     * while replaying (NULL otherwise), row the row being stored (NULL for a
     * del). Returned string is freed by the store. */
    char *(*row_key)(void *ctx, int coll, const char *id, const json *rec, const json *row);
    /** Append extra journal fields (`,"k":v`) for a record; they are kept in
     * the journal only, never in the public change. */
    void (*journal_extra)(void *ctx, int coll, const char *id, const char *key, const json *row, sbuf *out);
    /** Serialize a row for clients (MX_COLL_REDACT collections). NULL = json_write(). */
    void (*write_public)(void *ctx, int coll, const json *row, sbuf *out);
    /** Opaque scope for a ring entry; freed with scope_free. */
    void *(*scope_new)(void *ctx, mx_store *st, const mx_commit_info *ci);
    /** Frees a value scope_new returned. */
    void (*scope_free)(void *ctx, void *scope);
    /** After a row was stored / removed (live and replay). old is still valid. */
    void (*on_apply)(void *ctx, mx_store *st, int coll, const char *key, const json *old, const json *row);
    /** Right before a snapshot is serialized (e.g. prune tombstones). */
    void (*before_snapshot)(void *ctx, mx_store *st);
} mx_store_hooks;

/** Called after a change became durable, in seq order, for every public
 * change. @p ch is borrowed (it stays in the ring). */
typedef void (*mx_store_listener)(void *ctx, const mx_change *ch);

/** Store state. Applications read the fields (stats, recovery facts) and set
 * listener, listener_ctx, app, compact_threshold and sync_compaction; the
 * rest is internal. */
struct mx_store {
    char *dir;             /**< data directory */
    char *journal_path;    /**< `<dir>/journal.log` */
    char *snapshot_path;   /**< `<dir>/snapshot.json` */
    int journal_fd;        /**< append-only journal descriptor */
    int64_t journal_size;  /**< bytes on disk */
    int64_t seq;           /**< last assigned seq */
    int64_t synced_seq;    /**< last seq known durable and broadcast */
    int ncoll;             /**< number of collections */
    const mx_coll_def *defs; /**< collection definitions (borrowed) */
    hmap *colls;           /**< ncoll maps */
    mx_index *indexes;     /**< from the collection definitions */
    int nindexes;          /**< entries in indexes */
    mx_change *ring;       /**< retained public changes (ring_cap slots) */
    size_t ring_cap, ring_start, ring_len; /**< ring capacity, oldest slot, entries held */
    mx_store_listener listener; /**< durable-change callback (NULL = none) */
    void *listener_ctx;    /**< ctx for listener */
    mx_store_hooks hooks;  /**< copy of the hooks given to open */
    int64_t compact_threshold; /**< bytes; journal is compacted when it grows past this (default 4 MiB) */
    int recovered_truncated;   /**< set when a torn tail was cut off journal.log on open */
    int64_t skipped_records;   /**< journal records of unknown collections skipped on open (a newer
                                  version wrote them; they are dropped at the next compaction) */
    void *app;                 /**< free for the application */
    int loading;               /**< 1 while on_apply sees the rows of the snapshot being loaded */
    /* stats */
    int64_t open_ms;           /**< time open took (load + replay) */
    int64_t last_compact_ms;   /**< duration of the last compaction (parent-side) */
    int64_t fsyncs;            /**< number of journal fsyncs */
    int64_t writes;            /**< number of journal write batches */
    int64_t max_batch;         /**< most changes made durable by one fsync */
    char *journal_old_path;    /**< journal rotated away by a running background compaction */
    pid_t compact_pid;         /**< background compaction child (0 = none) */
    int64_t compact_started;   /**< when the running background compaction started (monotonic ms) */
    int sync_compaction;       /**< 1 = compact in-process (blocks the loop), 0 = fork (default) */
    int test_crash_compaction; /**< testing: the compaction child dies halfway through the snapshot */
    int64_t compactions, compact_failures; /**< compactions finished / failed */
    int64_t last_compact_block_ms; /**< how long the last compaction blocked the caller */
    int fault_syncs;           /**< testing: the next sync fails like an fsync error would (the store fails) */
    sbuf pending;              /**< group commit: records not written yet */
    int64_t pending_records;   /**< records in pending */
    /* sticky failure (see mx_store_failed) */
    int failed;                /**< 1 after a journal write / fsync error: read-only until reopened */
    int fail_errno;            /**< errno of that error (0 = injected through fault_syncs) */
    const char *fail_op;       /**< "write" or "fsync" (static string), NULL while healthy */
    int64_t rejected_writes;   /**< mutations refused since the failure */
    int rejected_pending;      /**< a mutation was refused since the last mx_store_sync */
    /* recovery (set by open) */
    int salvage;               /**< opened in salvage mode (mx_store_opts::salvage or MODX_STORE_SALVAGE=1) */
    int64_t recovered_tail_bytes; /**< bytes of torn tail cut off the journal */
    int64_t salvaged_records;  /**< salvage: damaged lines / out-of-order records skipped */
    int64_t salvaged_bytes;    /**< salvage: bytes they took */
    int64_t seq_gaps;          /**< salvage: seq gaps accepted */
    int snapshot_crc_mismatch; /**< salvage: the snapshot failed its checksum but parsed */
    int snapshot_unchecked;    /**< the snapshot was rewritten outside modx (length differs): loaded unchecked */
    char *recovery_report;     /**< salvage: path of the report written (NULL = nothing was damaged) */
};

/** mx_store_sync()'s answer once the store has failed (and mutations' errno = EROFS). */
#define MX_STORE_EFAILED (-2)

/** Health of the store, for /health or stats endpoints (mx_store_health_get()). */
typedef struct {
    int failed;                /**< 1: read-only after a journal I/O error; reopen (restart) to recover */
    int fail_errno;            /**< errno of that error (0 = injected) */
    const char *fail_op;       /**< "write" / "fsync" / NULL */
    int64_t seq, synced_seq;   /**< last assigned / last durable seq; synced_seq stops moving at the failure */
    int64_t pending_records;   /**< records queued, not written yet */
    int64_t rejected_writes;   /**< mutations refused since the failure */
    int64_t journal_size;      /**< journal bytes on disk */
} mx_store_health;

/** Open options (mx_store_open_ex()). Zero = defaults. */
typedef struct {
    /** Recover from damage a crash cannot cause instead of refusing to open:
     * keep every valid journal record before and after damaged ones, accept
     * seq gaps, skip records whose seq goes backwards, load a snapshot that
     * fails its checksum if it still parses. The damaged files are kept as
     * `<file>.damaged-<unix ms>`, a report is written, and the recovered state
     * is compacted into a fresh snapshot + empty journal. The environment
     * variable MODX_STORE_SALVAGE=1 turns this on as well (for an operator
     * restart). */
    int salvage;
    const char *report_path;   /**< salvage report (NULL = `<dir>/recovery-<unix ms>.txt`) */
} mx_store_opts;

/** Opens (creating if needed) the data dir, loads the snapshot, replays
 * journal.old (left by an interrupted background compaction) and journal.log.
 *
 * Recovery: a crash can only tear the end of the live journal, so the bytes
 * after its last record with a valid CRC are cut off (logged; see
 * mx_store::recovered_truncated / mx_store::recovered_tail_bytes). Anything
 * else fails the open with a message in @p err, because replaying past it
 * would silently drop data: a bad record followed by valid ones, a seq gap or
 * a seq going backwards after the snapshot's seq, damage in journal.old, a
 * snapshot that fails its checksum or does not parse. (A snapshot rewritten
 * by another tool, e.g. an operator's edit, has another length than its
 * header records: it is loaded without the check, with a warning;
 * mx_store::snapshot_unchecked.) Salvage (mx_store_opts, or
 * MODX_STORE_SALVAGE=1) recovers what is valid instead. Journals and
 * snapshots written by 0.1 / 0.2 (snapshots without a checksum) open
 * unchanged.
 *
 * @param defs     ncoll collection definitions; must outlive the store
 * @param ring_cap retained public changes for catch-up
 * @param hooks    copied; may be NULL
 * @return the store (free with mx_store_close()), or NULL with a message in
 *         @p err on fatal errors. */
mx_store *mx_store_open(const char *dir, const mx_coll_def *defs, int ncoll, size_t ring_cap,
                        const mx_store_hooks *hooks, char *err, size_t errlen);
/** mx_store_open() with options (@p opts may be NULL = defaults). */
mx_store *mx_store_open_ex(const char *dir, const mx_coll_def *defs, int ncoll, size_t ring_cap,
                           const mx_store_hooks *hooks, const mx_store_opts *opts, char *err, size_t errlen);
/** Waits for a running background compaction, writes and fsyncs queued
 * records (unless the store has failed; the listener is not called), and
 * frees everything. NULL is a no-op. */
void mx_store_close(mx_store *st);

/** Collection number of @p name, or -1. */
int mx_store_coll(const mx_store *st, const char *name);
/** Name of collection @p coll, or NULL when out of range. */
const char *mx_store_coll_name(const mx_store *st, int coll);
/** 1 when no change was ever made (seq 0) and every collection is empty. */
int mx_store_is_empty(const mx_store *st);

/** Row stored under @p key (borrowed; valid until the row is replaced or
 * removed), or NULL. Lookups by storage key return borrowed pointers. */
const json *mx_store_get(const mx_store *st, int coll, const char *key);
/** Rows in @p coll (must be a valid collection number). */
size_t mx_store_count(const mx_store *st, int coll);
/** All rows of @p coll (valid collection number), count in @p n: malloc'ed
 * array of borrowed pointers (caller frees the array only). Order is
 * unspecified. */
const json **mx_store_all(const mx_store *st, int coll, size_t *n);

/** Rows of @p coll whose string @p field equals @p value (borrowed pointers
 * in a malloc'ed array, count in @p n; caller frees the array, also when n
 * is 0). Uses the index when the field is declared in mx_coll_def::index,
 * else scans. Order is unspecified. */
const json **mx_store_find(const mx_store *st, int coll, const char *field, const char *value, size_t *n);
/** Number of rows mx_store_find() would return (O(1) for indexed fields). */
size_t mx_store_find_count(const mx_store *st, int coll, const char *field, const char *value);
/** 1 when (@p coll, @p field) is indexed. */
int mx_store_indexed(const mx_store *st, int coll, const char *field);

/** Stores @p obj in @p coll (journal, apply in memory, record in the ring;
 * not durable until mx_store_sync()). Takes ownership of obj (which needs a
 * string "id"), also on failure; the key comes from mx_store_hooks::row_key.
 * @return 0, or -1 (bad collection / id, or the store has failed: errno =
 *         EROFS, nothing applied; see mx_store_failed()). */
int mx_store_put(mx_store *st, int coll, json *obj, const char *client_id);
/** Removes the row stored under key @p id (row_key is not consulted; use
 * mx_store_del_key() for rows with another storage key). Returns like
 * mx_store_put(). */
int mx_store_del(mx_store *st, int coll, const char *id, const char *client_id);
/** Removes the row stored under @p key (NULL = @p id); @p id is what the
 * change reports. Returns like mx_store_put(). */
int mx_store_del_key(mx_store *st, int coll, const char *id, const char *key, const char *client_id);

/** Group commit: mutations only queue their journal records. mx_store_sync
 * writes everything queued with one write(), fsyncs once and then notifies
 * the listener of every newly durable change. May start a compaction.
 * @return 0; -1 when the write or the fsync fails (nothing of the batch is
 *         reported durable, and the store fails: see mx_store_failed());
 *         MX_STORE_EFAILED (-2) on every call after that. */
int mx_store_sync(mx_store *st);
/** 1 once the store has failed (sticky).
 *
 * A journal write() or fsync() error makes the store read-only for good: the
 * queued records are dropped, nothing is ever announced durable again (a
 * later fsync that "succeeds" proves nothing once one failed), fsync is never
 * retried, mutations return -1 (EROFS) and compaction is refused (memory
 * still holds the lost batch). Reads keep working; they may see changes of
 * the failed batch (their writers got an error: outcome unknown). The
 * catch-up feed stops at synced_seq. Recovery is a reopen (restart), which
 * replays exactly what reached the disk. With http_server's barrier a failed
 * commit turns the held responses into 500s, and requests that tried to write
 * afterwards into 503s. */
int mx_store_failed(const mx_store *st);
/** Fills @p h from the store. */
void mx_store_health_get(const mx_store *st, mx_store_health *h);
/** Adapter for http_server's durability barrier (ctx = the store): mx_store_dirty(). */
int mx_store_pending_cb(void *st);
/** Adapter for http_server's durability barrier (ctx = the store): mx_store_sync(). */
int mx_store_commit_cb(void *st);
/** 1 when changes were committed that are not durable yet. After a failure:
 * 1 when a mutation was refused since the last mx_store_sync() (so the
 * barrier answers the request that made it). */
int mx_store_dirty(const mx_store *st);

/** Write the snapshot (tmp + fsync + rename) and truncate the journal, in
 * this process (blocks until done). Waits for a running background one first.
 * @return 0, or -1 (I/O error, or the store has failed). */
int mx_store_compact(mx_store *st);
/** Background compaction: rotate journal.log to journal.old (fresh journal
 * for new records), fork, and let the child write the snapshot from its
 * copy-on-write image; mx_store_tick() reaps it and drops journal.old. The
 * parent is blocked only for the rotation and fork(). A crash anywhere in
 * between loses nothing: open replays snapshot + journal.old + journal.log
 * and merges them.
 * @return 0 (started), 1 (one is already running), -1. */
int mx_store_compact_bg(mx_store *st);

/** Filter for mx_store_for_each_since(): nonzero keeps @p ch. */
typedef int (*mx_change_filter)(void *ctx, const mx_change *ch);
/** Visitor for mx_store_for_each_since(); @p ch is borrowed. */
typedef void (*mx_change_fn)(void *ctx, const mx_change *ch);
/** Calls @p fn for every durable retained public change with seq > @p since
 * that @p keep accepts (keep NULL = all).
 * @return 0, or -1 when since is outside the retained window or ahead of
 *         the store's seq (data dir reset): the client must re-bootstrap. */
int mx_store_for_each_since(const mx_store *st, int64_t since, mx_change_filter keep, void *keep_ctx,
                            mx_change_fn fn, void *ctx);
/** i-th retained change, oldest first (@p i < ring_len; borrowed). */
const mx_change *mx_store_ring_at(const mx_store *st, size_t i);

/** Event loop housekeeping (reaps a background compaction). Cheap; call it
 * from the server tick. */
void mx_store_tick(mx_store *st);

#endif
