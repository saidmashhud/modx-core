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
    RUN(t_static);
}
