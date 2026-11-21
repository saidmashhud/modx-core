#include "modx/gzip.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef MODX_NO_ZLIB
#include <zlib.h>

int mx_gzip_available(void) { return 1; }

int mx_gzip(const void *in, size_t n, sbuf *out, int level) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, level > 0 && level <= 9 ? level : 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return -1;
    size_t start = out->len;
    size_t bound = deflateBound(&z, (uLong)n);
    sbuf_reserve(out, bound);
    z.next_in = (Bytef *)(uintptr_t)in;
    z.avail_in = (uInt)n;
    z.next_out = (Bytef *)out->data + out->len;
    z.avail_out = (uInt)bound;
    int rc = deflate(&z, Z_FINISH);
    if (rc != Z_STREAM_END) { deflateEnd(&z); out->len = start; out->data[start] = 0; return -1; }
    out->len += z.total_out;
    out->data[out->len] = 0;
    deflateEnd(&z);
    return 0;
}

int mx_gunzip(const void *in, size_t n, sbuf *out) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, 15 + 16) != Z_OK) return -1;
    z.next_in = (Bytef *)(uintptr_t)in;
    z.avail_in = (uInt)n;
    int rc;
    do {
        sbuf_reserve(out, 65536);
        z.next_out = (Bytef *)out->data + out->len;
        z.avail_out = 65536;
        rc = inflate(&z, Z_NO_FLUSH);
        out->len = (size_t)((char *)z.next_out - out->data);
        out->data[out->len] = 0;
    } while (rc == Z_OK);
    inflateEnd(&z);
    return rc == Z_STREAM_END ? 0 : -1;
}
#else
int mx_gzip_available(void) { return 0; }
int mx_gzip(const void *in, size_t n, sbuf *out, int level) { (void)in; (void)n; (void)out; (void)level; return -1; }
int mx_gunzip(const void *in, size_t n, sbuf *out) { (void)in; (void)n; (void)out; return -1; }
#endif

int mx_accepts_gzip(const char *ae) {
    if (!ae) return 0;
    const char *p = ae;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *tok = p;
        while (*p && *p != ',' && *p != ';' && *p != ' ') p++;
        size_t tl = (size_t)(p - tok);
        double q = 1;
        while (*p && *p != ',') {
            if (*p == ';') {
                p++;
                while (*p == ' ') p++;
                if ((*p == 'q' || *p == 'Q') && p[1] == '=') q = strtod(p + 2, NULL);
            } else {
                p++;
            }
        }
        if ((tl == 4 && strncasecmp(tok, "gzip", 4) == 0) || (tl == 1 && *tok == '*')) return q > 0;
    }
    return 0;
}

int mx_compressible(const char *ct) {
    if (!ct) return 0;
    return strncasecmp(ct, "text/", 5) == 0 || strncasecmp(ct, "application/json", 16) == 0 ||
           strncasecmp(ct, "application/javascript", 22) == 0 || strncasecmp(ct, "image/svg+xml", 13) == 0 ||
           strncasecmp(ct, "application/xml", 15) == 0 || strncasecmp(ct, "application/manifest+json", 25) == 0;
}
