#include "modx/cache.h"
#include "modx/util.h"

#include <stdlib.h>
#include <string.h>

void mx_cache_init(mx_cache *c, size_t max_bytes) {
    memset(c, 0, sizeof *c);
    c->max_bytes = max_bytes;
}

static void drop(mx_cache *c, size_t i) {
    c->bytes -= c->e[i].len + strlen(c->e[i].key);
    free(c->e[i].key);
    free(c->e[i].body);
    c->e[i] = c->e[--c->n];
}

void mx_cache_clear(mx_cache *c) {
    while (c->n) drop(c, c->n - 1);
}

void mx_cache_free(mx_cache *c) {
    mx_cache_clear(c);
    free(c->e);
    c->e = NULL;
    c->cap = 0;
}

static long find(const mx_cache *c, const char *key) {
    for (size_t i = 0; i < c->n; i++)
        if (strcmp(c->e[i].key, key) == 0) return (long)i;
    return -1;
}

const char *mx_cache_get(mx_cache *c, const char *key, int64_t seq, size_t *len) {
    long i = find(c, key);
    if (i < 0 || c->e[i].seq != seq) {
        c->misses++;
        if (i >= 0) drop(c, (size_t)i); /* stale */
        return NULL;
    }
    c->hits++;
    c->e[i].used = ++c->clock;
    if (len) *len = c->e[i].len;
    return c->e[i].body;
}

void mx_cache_put(mx_cache *c, const char *key, int64_t seq, const char *body, size_t len) {
    size_t cost = len + strlen(key);
    if (!c->max_bytes || cost > c->max_bytes / 4) return;
    long i = find(c, key);
    if (i >= 0) drop(c, (size_t)i);
    while (c->n && c->bytes + cost > c->max_bytes) {
        size_t lru = 0;
        for (size_t k = 1; k < c->n; k++)
            if (c->e[k].used < c->e[lru].used) lru = k;
        drop(c, lru);
    }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->e = xrealloc(c->e, c->cap * sizeof *c->e);
    }
    mx_cache_entry *e = &c->e[c->n++];
    e->key = xstrdup(key);
    e->body = xmalloc(len + 1);
    memcpy(e->body, body, len);
    e->body[len] = 0;
    e->len = len;
    e->seq = seq;
    e->used = ++c->clock;
    c->bytes += cost;
}
