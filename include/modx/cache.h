#ifndef MODX_CACHE_H
#define MODX_CACHE_H

/* A small response cache with seq invalidation: an entry is valid only for
 * the store seq it was built at, so any committed change makes it stale
 * without explicit invalidation. Bounded by bytes, least recently used
 * entries go first. Meant for expensive, often repeated reads (an app's
 * bootstrap payload per user, a list endpoint). */

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *key;
    char *body;
    size_t len;
    int64_t seq;
    int64_t used;
} mx_cache_entry;

typedef struct {
    mx_cache_entry *e;
    size_t n, cap;
    size_t bytes, max_bytes;
    int64_t clock;
    int64_t hits, misses;
} mx_cache;

void mx_cache_init(mx_cache *c, size_t max_bytes);
void mx_cache_free(mx_cache *c);
/* Body cached under key at exactly seq (borrowed until the next put), or NULL. */
const char *mx_cache_get(mx_cache *c, const char *key, int64_t seq, size_t *len);
/* Store a copy of body under key for seq (replaces an older entry). Bodies
 * larger than max_bytes / 4 are not cached. */
void mx_cache_put(mx_cache *c, const char *key, int64_t seq, const char *body, size_t len);
void mx_cache_clear(mx_cache *c);

#endif
