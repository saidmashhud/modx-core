/* HTTP/1.1 request parser: request line, headers, query decoding, bodies,
 * limits, Expect: 100-continue, pipelining; static path safety and MIME. */
#include <stdlib.h>

#include "modx/http.h"
#include "modx/static.h"
#include "test.h"

static long parse(const char *raw, http_request *r) {
    int expect;
    return http_parse_request(raw, strlen(raw), r, &expect);
}

static size_t big_limit(const http_request *partial, void *ctx) {
    (void)ctx;
    return strcmp(partial->path, "/upload") == 0 ? 4 * 1024 * 1024 : HTTP_MAX_BODY;
}

static void t_parse(void) {
    http_request r;
    const char *raw = "GET /api/items%20x?a=1&b=two+words&c=%2Fz&flag HTTP/1.1\r\nHost: x\r\nX-Thing:  v  \r\n\r\n";
    CHECK(parse(raw, &r) == (long)strlen(raw));
    CHECK_STR(r.method, "GET");
    CHECK_STR(r.path, "/api/items x");
    CHECK_STR(http_req_query(&r, "b"), "two words");
    CHECK_STR(http_req_query(&r, "c"), "/z");
    CHECK_STR(http_req_query(&r, "flag"), "");
    CHECK(http_req_query(&r, "missing") == NULL);
    CHECK_STR(http_req_header(&r, "x-thing"), "v");
    CHECK(r.keep_alive == 1);
    http_request_free(&r);

    raw = "POST /p HTTP/1.0\r\nContent-Length: 5\r\n\r\nhello";
    CHECK(parse(raw, &r) == (long)strlen(raw));
    CHECK(r.body_len == 5 && memcmp(r.body, "hello", 5) == 0 && r.keep_alive == 0);
    http_request_free(&r);

    /* incomplete: headers, then body */
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n", &r) == 0);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\nabc", &r) == 0);
    /* pipelined: only the first request is consumed */
    raw = "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
    CHECK(parse(raw, &r) == 19);
    CHECK_STR(r.path, "/a");
    http_request_free(&r);
}

static void t_errors(void) {
    http_request r;
    CHECK(parse("GET / HTTP/2.0\r\n\r\n", &r) == -400);
    CHECK(parse("get / HTTP/1.1\r\n\r\n", &r) == -400);
    CHECK(parse("GET relative HTTP/1.1\r\n\r\n", &r) == -400);
    CHECK(parse("GET  HTTP/1.1\r\n\r\n", &r) == -400);
    CHECK(parse("GET /%00 HTTP/1.1\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nNoColon\r\n\r\n", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 1x\r\n\r\n", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &r) == -411);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 2000000\r\n\r\n", &r) == -413);
    CHECK(parse("POST / HTTP/1.1\r\nContent-Length: 99999999999\r\n\r\n", &r) == -413);
    /* header block too large */
    sbuf b;
    sbuf_init(&b);
    sbuf_puts(&b, "GET / HTTP/1.1\r\nX: ");
    for (int i = 0; i < HTTP_MAX_HEADER; i++) sbuf_putc(&b, 'a');
    int expect;
    CHECK(http_parse_request(b.data, b.len, &r, &expect) == -431);
    sbuf_free(&b);
    /* per-route body limit */
    const char *up = "POST /upload HTTP/1.1\r\nContent-Length: 2000000\r\nExpect: 100-continue\r\n\r\n";
    size_t need = 0;
    CHECK(http_parse_request_need(up, strlen(up), &r, &expect, big_limit, NULL, &need) == 0);
    CHECK(expect == 1 && need == strlen(up) + 2000000);
    const char *no = "POST /other HTTP/1.1\r\nContent-Length: 2000000\r\n\r\n";
    CHECK(http_parse_request_ex(no, strlen(no), &r, &expect, big_limit, NULL) == -413);
}

/* Framing that a proxy and this parser could read differently (request
 * smuggling) is rejected, never ignored. */
static void t_strict_framing(void) {
    http_request r;
    /* whitespace between field name and colon (RFC 9112 5.1) */
    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length : 5\r\n\r\nhello", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length\t: 5\r\n\r\nhello", &r) == -400);
    /* the smuggling case: a pipelined second request hidden in the "ignored" body */
    const char *smug = "POST /a HTTP/1.1\r\nHost: x\r\nContent-Length : 32\r\n\r\n"
                       "GET /admin HTTP/1.1\r\nHost: x\r\n\r\n"
                       "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(parse(smug, &r) == -400);
    /* names must be tokens */
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n X-Fold: a\r\n\r\n", &r) == -400);   /* obs-fold */
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n\tX: a\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX(y): a\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX\"y: a\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX/y: a\r\n\r\n", &r) == -400);
    const char *ok = "GET / HTTP/1.1\r\nHost: x\r\nX-A_b.c~!#$%&'*+^`|: v\r\n\r\n";
    CHECK(parse(ok, &r) == (long)strlen(ok));
    CHECK_STR(http_req_header(&r, "x-a_b.c~!#$%&'*+^`|"), "v");
    http_request_free(&r);
    /* bare CR / LF anywhere in the header block */
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX: a\rContent-Length: 5\r\n\r\nhello", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX: a\r\r\n\r\n", &r) == -400);
    CHECK(parse("GET /\r HTTP/1.1\r\nHost: x\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\rHost: x\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX: a\nContent-Length: 5\r\n\r\nhello", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\nHost: x\r\n\r\n", &r) == -400);
    /* control characters in the request-target and in values (HTAB is allowed in values) */
    CHECK(parse("GET /a\x01b HTTP/1.1\r\nHost: x\r\n\r\n", &r) == -400);
    CHECK(parse("GET /a\x7f HTTP/1.1\r\nHost: x\r\n\r\n", &r) == -400);
    CHECK(parse("GET /a?q=\x1b HTTP/1.1\r\nHost: x\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX: a\x01b\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX: a\x7f\r\n\r\n", &r) == -400);
    int expect;
    static const char nul[] = "GET / HTTP/1.1\r\nHost: x\r\nX: a\0b\r\n\r\n";
    CHECK(http_parse_request(nul, sizeof nul - 1, &r, &expect) == -400);
    static const char nul_t[] = "GET /a\0b HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(http_parse_request(nul_t, sizeof nul_t - 1, &r, &expect) == -400);
    const char *tab = "GET / HTTP/1.1\r\nHost: x\r\nX: a\tb \xc3\xa9\r\n\r\n";
    CHECK(parse(tab, &r) == (long)strlen(tab));
    CHECK_STR(http_req_header(&r, "x"), "a\tb \xc3\xa9");
    http_request_free(&r);
}

static void t_static(void) {
    CHECK(static_path_safe("/a/b.js"));
    CHECK(static_path_safe("/"));
    CHECK(!static_path_safe("/../etc/passwd"));
    CHECK(!static_path_safe("/a/../../b"));
    CHECK(!static_path_safe("/./a"));
    CHECK(!static_path_safe("a"));
    CHECK(!static_path_safe("/a\\b"));
    CHECK(static_path_safe("/a..b/c"));
    CHECK_STR(static_mime_type("/x/app.JS"), "text/javascript; charset=utf-8");
    CHECK_STR(static_mime_type("/x.woff2"), "font/woff2");
    CHECK_STR(static_mime_type("/noext"), "application/octet-stream");
    char *d = http_url_decode("a%2bb+c", 7, 1);
    CHECK_STR(d, "a+b c");
    free(d);
    CHECK(http_url_decode("%00", 3, 0) == NULL);
}

void test_http_all(void) {
    RUN(t_parse);
    RUN(t_errors);
    RUN(t_strict_framing);
    RUN(t_static);
}
