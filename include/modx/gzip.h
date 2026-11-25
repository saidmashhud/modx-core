#ifndef MODX_GZIP_H
#define MODX_GZIP_H

/**
 * @file gzip.h
 * @brief gzip response compression through the system zlib (build with ZLIB=0 to
 * leave it out; everything then reports "unavailable" and responses go out
 * uncompressed).
 */

#include <stddef.h>
#include "modx/util.h"

/** 1 when compression is compiled in. */
int mx_gzip_available(void);
/** Append the gzip encoding of in[0..n) to out. level 1..9 (other values = default 6).
 * Returns 0, or -1 (unavailable / zlib error; out unchanged). */
int mx_gzip(const void *in, size_t n, sbuf *out, int level);
/** Inflate a gzip stream, appending to @p out (tests, clients). Returns 0, or -1
 * (unavailable, corrupt or truncated input; out may hold partial output). */
int mx_gunzip(const void *in, size_t n, sbuf *out);
/** 1 when an Accept-Encoding header allows gzip, directly or via "*" (honours "gzip;q=0"). NULL = 0. */
int mx_accepts_gzip(const char *accept_encoding);
/** 1 for content types worth compressing (text, JSON, JS, CSS, SVG, XML, web manifest). */
int mx_compressible(const char *content_type);

#endif
