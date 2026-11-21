/* Fuzz target: HTTP/1.1 request parser. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "modx/http.h"

static size_t limit(const http_request *partial, void *ctx) {
    (void)ctx;
    return partial->path && strncmp(partial->path, "/up", 3) == 0 ? 4 * 1024 * 1024 : HTTP_MAX_BODY;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* the server keeps input NUL-terminated */
    char *buf = malloc(size + 1);
    memcpy(buf, data, size);
    buf[size] = 0;
    size_t off = 0;
    for (int k = 0; k < 8 && off <= size; k++) { /* pipelined requests */
        http_request r;
        int expect;
        size_t need = 0;
        long n = http_parse_request_need(buf + off, size - off, &r, &expect, limit, NULL, &need);
        if (n <= 0) break;
        if ((size_t)n > size - off || r.body_len > (size_t)n) abort();
        if (!r.path || r.path[0] != '/' || !r.target) abort();
        for (size_t i = 0; i < r.nheaders; i++)
            for (const char *c = r.headers[i].key; *c; c++)
                if (*c >= 'A' && *c <= 'Z') abort(); /* header names are lower-cased */
        (void)http_req_header(&r, "host");
        (void)http_req_query(&r, "a");
        http_request_free(&r);
        off += (size_t)n;
    }
    char *d = http_url_decode(buf, size, 1);
    free(d);
    free(buf);
    return 0;
}
