#include "modx/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
    return p;
}

void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) { fprintf(stderr, "out of memory\n"); abort(); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "out of memory\n"); abort(); }
    return q;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void sbuf_init(sbuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }

void sbuf_free(sbuf *b) { free(b->data); sbuf_init(b); }

void sbuf_reserve(sbuf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    int fresh = b->data == NULL;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
    if (fresh) b->data[0] = 0; /* keep the "always NUL-terminated" invariant for empty buffers */
}

void sbuf_reserve_exact(sbuf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    int fresh = b->data == NULL;
    b->data = xrealloc(b->data, need);
    b->cap = need;
    if (fresh) b->data[0] = 0;
}

void sbuf_shrink(sbuf *b) {
    if (!b->data || b->cap <= b->len + 1) return;
    size_t cap = b->len + 1 < 64 ? 64 : b->len + 1;
    if (cap >= b->cap) return;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

void sbuf_put(sbuf *b, const void *p, size_t n) {
    sbuf_reserve(b, n);
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

void sbuf_puts(sbuf *b, const char *s) { sbuf_put(b, s, strlen(s)); }

void sbuf_putc(sbuf *b, char c) { sbuf_put(b, &c, 1); }

void sbuf_printf(sbuf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    if ((size_t)n < sizeof tmp) {
        sbuf_put(b, tmp, (size_t)n);
    } else {
        sbuf_reserve(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

void sbuf_consume(sbuf *b, size_t n) {
    if (n >= b->len) { b->len = 0; if (b->data) b->data[0] = 0; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    b->data[b->len] = 0;
}

char *sbuf_take(sbuf *b) {
    char *p = b->data;
    if (!p) p = xstrdup("");
    sbuf_init(b);
    return p;
}

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
    crc_ready = 1;
}

uint32_t crc32_buf(const void *data, size_t len) {
    if (!crc_ready) crc_init();
    const unsigned char *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t rng_state;

uint64_t rand_u64(void) {
    if (!rng_state) {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            if (read(fd, &rng_state, sizeof rng_state) != (ssize_t)sizeof rng_state) rng_state = 0;
            close(fd);
        }
        rng_state ^= (uint64_t)now_ms() * 6364136223846793005ULL ^ (uint64_t)getpid();
        if (!rng_state) rng_state = 0x9E3779B97F4A7C15ULL;
    }
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

char *gen_id(const char *prefix) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tbuf[32];
    int n = 0;
    uint64_t t = (uint64_t)now_ms();
    do { tbuf[n++] = digits[t % 36]; t /= 36; } while (t && n < 30);
    sbuf b;
    sbuf_init(&b);
    sbuf_puts(&b, prefix);
    sbuf_putc(&b, '_');
    while (n > 0) sbuf_putc(&b, tbuf[--n]);
    uint64_t r = rand_u64();
    for (int i = 0; i < 4; i++) { sbuf_putc(&b, digits[r % 36]); r /= 36; }
    return sbuf_take(&b);
}

int str_icase_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

int str_icase_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    for (const char *h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

long str_icase_find(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 0;
    for (const char *h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return (long)(h - hay);
    }
    return -1;
}

size_t utf8_count(const char *s, size_t n) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) k++;
    return k;
}

/* Byte offset after advancing `chars` code points from byte offset `from`. */
static size_t utf8_advance(const char *s, size_t n, size_t from, size_t chars) {
    size_t i = from;
    while (i < n && chars) {
        i++;
        while (i < n && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
        chars--;
    }
    return i;
}

char *text_snippet(const char *text, size_t match_off, size_t max_chars) {
    size_t n = strlen(text);
    size_t total = utf8_count(text, n);
    if (total <= max_chars || max_chars < 3) return xstrndup(text, total <= max_chars ? n : utf8_advance(text, n, 0, max_chars));
    if (match_off > n) match_off = n;
    size_t before = utf8_count(text, match_off); /* code points before the match */
    size_t lead = 40;                            /* context kept before the match */
    size_t start_cp = before > lead ? before - lead : 0;
    size_t budget;
    if (start_cp == 0) {
        budget = max_chars - 1; /* only the right side is cut */
    } else if (start_cp + (max_chars - 1) >= total) {
        start_cp = total - (max_chars - 1); /* only the left side is cut */
        budget = max_chars - 1;
    } else {
        budget = max_chars - 2;
    }
    size_t s = utf8_advance(text, n, 0, start_cp);
    size_t e = utf8_advance(text, n, s, budget);
    sbuf b;
    sbuf_init(&b);
    if (s > 0) sbuf_puts(&b, "\xe2\x80\xa6");
    sbuf_put(&b, text + s, e - s);
    if (e < n) sbuf_puts(&b, "\xe2\x80\xa6");
    return sbuf_take(&b);
}
