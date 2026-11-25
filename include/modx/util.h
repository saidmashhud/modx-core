#ifndef MODX_UTIL_H
#define MODX_UTIL_H

/** @file util.h
 * @brief Byte buffers, aborting allocators, CRC-32, clocks, ids and small
 * string helpers.
 *
 * The x* allocators and every function that allocates through them print
 * "out of memory" and abort() instead of returning NULL.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/** Growable byte buffer. Always NUL-terminated when data != NULL.
 * Zero-initialized or sbuf_init() = empty (data NULL). */
typedef struct {
    char *data; /**< bytes (owned), NULL until the first write */
    size_t len; /**< bytes used, without the terminator */
    size_t cap; /**< bytes allocated */
} sbuf;

/** Makes @p b empty without freeing anything. */
void sbuf_init(sbuf *b);
/** Frees the data; @p b is empty afterwards. */
void sbuf_free(sbuf *b);
/** Ensures room for @p extra more bytes plus the terminator (capacity
 * doubles, starting at 64). */
void sbuf_reserve(sbuf *b, size_t extra);
/** Like sbuf_reserve(), but grows to exactly len + extra + 1 bytes (no
 * doubling): for buffers whose final size is known (a large request body). */
void sbuf_reserve_exact(sbuf *b, size_t extra);
/** Give back capacity beyond len + 1 (at least 64 bytes are kept; after a
 * large buffer was consumed). */
void sbuf_shrink(sbuf *b);
/** Appends @p n bytes of @p p. */
void sbuf_put(sbuf *b, const void *p, size_t n);
/** Appends a NUL-terminated string. */
void sbuf_puts(sbuf *b, const char *s);
/** Appends one byte. */
void sbuf_putc(sbuf *b, char c);
/** Appends printf-formatted text. */
void sbuf_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/** Drop the first @p n bytes (all of them when n >= len). */
void sbuf_consume(sbuf *b, size_t n);
/** Detach the buffer (caller frees); @p b is empty afterwards. Never returns
 * NULL (an empty buffer gives ""). */
char *sbuf_take(sbuf *b);

/** malloc() that aborts on failure; 0 bytes allocates 1. */
void *xmalloc(size_t n);
/** calloc() that aborts on failure. */
void *xcalloc(size_t n, size_t sz);
/** realloc() that aborts on failure. */
void *xrealloc(void *p, size_t n);
/** strdup() that aborts on failure. */
char *xstrdup(const char *s);
/** Copy of the first @p n bytes of @p s plus a NUL (no strlen; s must hold n bytes). */
char *xstrndup(const char *s, size_t n);

/** CRC-32 (IEEE 802.3, as zlib's crc32) of @p len bytes. */
uint32_t crc32_buf(const void *data, size_t len);

/** Wall-clock time in Unix milliseconds (CLOCK_REALTIME). */
int64_t now_ms(void);
/** Monotonic milliseconds (CLOCK_MONOTONIC), for durations. */
int64_t mono_ms(void);

/** Random 64-bit value (xorshift seeded from /dev/urandom). Not
 * cryptographic and not thread-safe: use crypto_random() for secrets. */
uint64_t rand_u64(void);
/** `"<prefix>_<base36 ms time><4 random base36 chars>"`; caller frees. */
char *gen_id(const char *prefix);

/** 1 when @p a and @p b are equal ignoring ASCII case. */
int str_icase_eq(const char *a, const char *b);
/** Case-insensitive substring search (ASCII folding). An empty needle matches. */
int str_icase_contains(const char *hay, const char *needle);

/** Byte offset of the first case-insensitive (ASCII folding) occurrence of
 * @p needle in @p hay, or -1 (0 for an empty needle). */
long str_icase_find(const char *hay, const char *needle);
/** Number of UTF-8 code points in @p n bytes (continuation bytes are not counted). */
size_t utf8_count(const char *s, size_t n);
/** At most @p max_chars code points of @p text around the match at byte
 * offset @p match_off, with "…" marking each cut side (counted in
 * max_chars). Caller frees. */
char *text_snippet(const char *text, size_t match_off, size_t max_chars);

#endif
