#ifndef MODX_CACHE_H
#define MODX_CACHE_H

/**
 * @file cache.h
 * @brief A small response cache with seq invalidation: an entry is valid only for
 * the store seq it was built at, so any committed change makes it stale
 * without explicit invalidation. Bounded by bytes, least recently used
 * entries go first. Meant for expensive, often repeated reads (an app's
 * bootstrap payload per user, a list endpoint).
 *
 * Not thread-safe. Lookups are linear in the number of entries.
 */

#include <stddef.h>
#include <stdint.h>

/** One cached body. */
typedef struct {
    char *key;    /**< Owned copy of the key. */
    char *body;   /**< Owned, NUL-terminated copy of the body. */
    size_t len;   /**< Body length. */
    int64_t seq;  /**< Store seq the body was built at. */
    int64_t used; /**< Logical clock of the last use (LRU). */
} mx_cache_entry;

/** The cache. */
typedef struct {
    mx_cache_entry *e;        /**< Entries. */
    size_t n, cap;            /**< Count and capacity of @c e. */
    size_t bytes, max_bytes;  /**< Bytes used (key + body lengths) and the bound. */
    int64_t clock;            /**< Logical clock for LRU. */
    int64_t hits, misses;     /**< Counters. */
} mx_cache;

/** Initialize an empty cache bounded to @p max_bytes (0 = caches nothing). */
void mx_cache_init(mx_cache *c, size_t max_bytes);
/** Free every entry and the array. */
void mx_cache_free(mx_cache *c);
/** Body cached under key at exactly seq (NUL-terminated, borrowed until the
 * next put, clear or miss), or NULL. A stale entry is dropped. *len (if non-NULL) gets the length. */
const char *mx_cache_get(mx_cache *c, const char *key, int64_t seq, size_t *len);
/** Store a copy of body under key for seq (replaces an older entry), evicting
 * least recently used entries to fit. Bodies (plus key) larger than max_bytes / 4 are not cached. */
void mx_cache_put(mx_cache *c, const char *key, int64_t seq, const char *body, size_t len);
/** Drop every entry (counters are kept). */
void mx_cache_clear(mx_cache *c);

#endif
