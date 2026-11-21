/* WebSocket pieces: SHA-1 and base64 vectors, the RFC 6455 accept key,
 * the frame parser (lengths, masks, control-frame rules, limits) and the
 * UTF-8 validator. The socket-level behaviour is in tests/todo.mjs. */
#include <stdlib.h>

#include "modx/crypto.h"
#include "modx/ws.h"
#include "test.h"

static void t_sha1_base64(void) {
    unsigned char h[SHA1_LEN];
    char hex[41];
    sha1("abc", 3, h);
    hex_encode(h, sizeof h, hex);
    CHECK_STR(hex, "a9993e364706816aba3e25717850c26c9cd0d89d");
    sha1("", 0, h);
    hex_encode(h, sizeof h, hex);
    CHECK_STR(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; /* 56 bytes: two blocks */
    sha1(two, strlen(two), h);
    hex_encode(h, sizeof h, hex);
    CHECK_STR(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    char b[64];
    base64_encode((const unsigned char *)"", 0, b);
    CHECK_STR(b, "");
    base64_encode((const unsigned char *)"f", 1, b);
    CHECK_STR(b, "Zg==");
    base64_encode((const unsigned char *)"fo", 2, b);
    CHECK_STR(b, "Zm8=");
    base64_encode((const unsigned char *)"foobar", 6, b);
    CHECK_STR(b, "Zm9vYmFy");
    unsigned char d[16];
    CHECK(base64_decode("Zm9vYmE=", d, sizeof d) == 5 && memcmp(d, "fooba", 5) == 0);
    CHECK(base64_decode("Zm9", d, sizeof d) == -1);
    CHECK(base64_decode("Zm=v", d, sizeof d) == -1);
    CHECK(base64_decode("Z!9v", d, sizeof d) == -1);
    CHECK(base64_decode("Zm8=Zm8=", d, sizeof d) == -1); /* padding only at the end */
    char acc[29];
    mx_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc); /* RFC 6455 section 1.3 */
    CHECK_STR(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

static void t_frames(void) {
    mx_ws_frame f;
    const unsigned char hello[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 }; /* RFC 5.7 masked "Hello" */
    CHECK(mx_ws_parse_frame(hello, sizeof hello, &f, 1000) == (long)sizeof hello);
    CHECK(f.fin && f.opcode == MX_WS_TEXT && f.masked && f.len == 5 && f.header == 6);
    CHECK(mx_ws_parse_frame(hello, 5, &f, 1000) == 0); /* incomplete */
    CHECK(mx_ws_parse_frame(hello, 1, &f, 1000) == 0);
    unsigned char m[14] = { 0x82, 0xFE, 0x01, 0x00 }; /* 256 bytes, 16-bit length, masked */
    CHECK(mx_ws_parse_frame(m, 8, &f, 1000) == 0 && f.len == 256);
    CHECK(mx_ws_parse_frame(m, 8, &f, 100) == -1009);
    unsigned char small16[] = { 0x82, 0xFE, 0x00, 0x05 };  /* 16-bit length for a 5-byte payload: not minimal */
    CHECK(mx_ws_parse_frame(small16, sizeof small16, &f, 1000) == -1002);
    unsigned char big[] = { 0x82, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0 }; /* top bit set */
    CHECK(mx_ws_parse_frame(big, sizeof big, &f, (size_t)-1) == -1002);
    unsigned char rsv[] = { 0xC1, 0x80, 0, 0, 0, 0 };
    CHECK(mx_ws_parse_frame(rsv, sizeof rsv, &f, 1000) == -1002);
    unsigned char op3[] = { 0x83, 0x80, 0, 0, 0, 0 };
    CHECK(mx_ws_parse_frame(op3, sizeof op3, &f, 1000) == -1002);
    unsigned char fragping[] = { 0x09, 0x80, 0, 0, 0, 0 };   /* control frames cannot be fragmented */
    CHECK(mx_ws_parse_frame(fragping, sizeof fragping, &f, 1000) == -1002);
    unsigned char longping[] = { 0x89, 0xFE, 0x00, 0x7E }; /* > 125 bytes */
    CHECK(mx_ws_parse_frame(longping, sizeof longping, &f, 1000) == -1002);
    unsigned char unmasked[] = { 0x81, 0x02, 'h', 'i' };
    CHECK(mx_ws_parse_frame(unmasked, sizeof unmasked, &f, 1000) == 4 && !f.masked); /* the server rejects it */
}

static void t_utf8(void) {
    CHECK(mx_utf8_valid((const unsigned char *)"plain", 5));
    CHECK(mx_utf8_valid((const unsigned char *)"\xd0\x9f\xd1\x80\xd0\xb8", 6));   /* Cyrillic */
    CHECK(mx_utf8_valid((const unsigned char *)"\xf0\x9f\x98\x80", 4));           /* emoji */
    CHECK(!mx_utf8_valid((const unsigned char *)"\xc3", 1));                      /* truncated */
    CHECK(!mx_utf8_valid((const unsigned char *)"\xc0\xaf", 2));                  /* overlong */
    CHECK(!mx_utf8_valid((const unsigned char *)"\xed\xa0\x80", 3));              /* surrogate */
    CHECK(!mx_utf8_valid((const unsigned char *)"\xf4\x90\x80\x80", 4));          /* > U+10FFFF */
    CHECK(!mx_utf8_valid((const unsigned char *)"\x80", 1));
    CHECK(!mx_utf8_valid((const unsigned char *)"a\xe2\x82", 3));
}

void test_ws_all(void) {
    RUN(t_sha1_base64);
    RUN(t_frames);
    RUN(t_utf8);
}
