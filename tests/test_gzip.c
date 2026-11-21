/* gzip helpers and the seq-invalidated response cache. */
#include <stdlib.h>

#include "modx/cache.h"
#include "modx/gzip.h"
#include "test.h"

static void t_gzip(void) {
    CHECK(mx_accepts_gzip("gzip, deflate, br"));
    CHECK(mx_accepts_gzip("br;q=1.0, GZIP;q=0.5"));
    CHECK(mx_accepts_gzip("*"));
    CHECK(!mx_accepts_gzip("gzip;q=0"));
    CHECK(!mx_accepts_gzip("deflate, br"));
    CHECK(!mx_accepts_gzip("xgzip"));
    CHECK(!mx_accepts_gzip(NULL));
    CHECK(mx_compressible("application/json; charset=utf-8"));
    CHECK(mx_compressible("text/html"));
    CHECK(!mx_compressible("image/png"));
    CHECK(!mx_compressible(NULL));
    if (!mx_gzip_available()) return;
    sbuf in, z, back;
    sbuf_init(&in);
    sbuf_init(&z);
    sbuf_init(&back);
    for (int i = 0; i < 2000; i++) sbuf_printf(&in, "{\"id\":\"row%d\",\"title\":\"hello\"},", i);
    CHECK(mx_gzip(in.data, in.len, &z, 0) == 0);
    CHECK(z.len > 18 && z.len < in.len / 5);
    CHECK((unsigned char)z.data[0] == 0x1f && (unsigned char)z.data[1] == 0x8b);
    CHECK(mx_gunzip(z.data, z.len, &back) == 0 && back.len == in.len && memcmp(back.data, in.data, in.len) == 0);
    back.len = 0;
    CHECK(mx_gunzip("not gzip", 8, &back) == -1);
    sbuf_free(&in);
    sbuf_free(&z);
    sbuf_free(&back);
}

static void t_cache(void) {
    mx_cache c;
    mx_cache_init(&c, 1000);
    size_t n;
    CHECK(mx_cache_get(&c, "a", 1, &n) == NULL);
    mx_cache_put(&c, "a", 1, "AAAA", 4);
    const char *v = mx_cache_get(&c, "a", 1, &n);
    CHECK(v && n == 4 && memcmp(v, "AAAA", 4) == 0);
    CHECK(mx_cache_get(&c, "a", 2, &n) == NULL);   /* any newer seq makes it stale */
    CHECK(mx_cache_get(&c, "a", 1, &n) == NULL);   /* and stale entries are dropped */
    char big[300];
    memset(big, 'x', sizeof big);
    mx_cache_put(&c, "big", 1, big, sizeof big);   /* > max/4: not cached */
    CHECK(mx_cache_get(&c, "big", 1, &n) == NULL);
    /* LRU eviction under the byte budget */
    char key[8], body[200];
    memset(body, 'y', sizeof body);
    for (int i = 0; i < 4; i++) { snprintf(key, sizeof key, "k%d", i); mx_cache_put(&c, key, 5, body, 200); }
    CHECK(mx_cache_get(&c, "k0", 5, &n) != NULL); /* touch k0 */
    mx_cache_put(&c, "k4", 5, body, 200);          /* evicts the least recently used: k1 */
    CHECK(mx_cache_get(&c, "k1", 5, &n) == NULL);
    CHECK(mx_cache_get(&c, "k0", 5, &n) != NULL && mx_cache_get(&c, "k4", 5, &n) != NULL);
    CHECK(c.bytes <= c.max_bytes);
    CHECK(c.hits >= 4 && c.misses >= 4);
    mx_cache_free(&c);
}

void test_gzip_all(void) {
    RUN(t_gzip);
    RUN(t_cache);
}
