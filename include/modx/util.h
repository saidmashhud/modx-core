#ifndef MODX_UTIL_H
#define MODX_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Growable byte buffer. Always NUL-terminated when data != NULL. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sbuf;

void sbuf_init(sbuf *b);
void sbuf_free(sbuf *b);
void sbuf_reserve(sbuf *b, size_t extra);
/* Like sbuf_reserve, but grows to exactly len + extra + 1 bytes (no doubling):
 * for buffers whose final size is known (a large request body). */
void sbuf_reserve_exact(sbuf *b, size_t extra);
/* Give back capacity beyond len + 1 (after a large buffer was consumed). */
void sbuf_shrink(sbuf *b);
void sbuf_put(sbuf *b, const void *p, size_t n);
void sbuf_puts(sbuf *b, const char *s);
void sbuf_putc(sbuf *b, char c);
void sbuf_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Drop the first n bytes. */
void sbuf_consume(sbuf *b, size_t n);
/* Detach the buffer (caller frees). Never returns NULL. */
char *sbuf_take(sbuf *b);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

uint32_t crc32_buf(const void *data, size_t len);

int64_t now_ms(void);
int64_t mono_ms(void);

/* Random 64-bit value (xorshift seeded from /dev/urandom). */
uint64_t rand_u64(void);
/* "<prefix>_<base36 ms time><4 random base36 chars>"; caller frees. */
char *gen_id(const char *prefix);

int str_icase_eq(const char *a, const char *b);
/* Case-insensitive substring search (ASCII folding). */
int str_icase_contains(const char *hay, const char *needle);

/* Byte offset of the first case-insensitive (ASCII folding) occurrence of
 * needle in hay, or -1. */
long str_icase_find(const char *hay, const char *needle);
/* Number of UTF-8 code points (continuation bytes are not counted). */
size_t utf8_count(const char *s, size_t n);
/* At most max_chars code points of text around the match at byte offset
 * match_off, with "…" marking each cut side (counted in max_chars). Caller frees. */
char *text_snippet(const char *text, size_t match_off, size_t max_chars);

#endif
