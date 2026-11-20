#include <stdlib.h>

#include "modx/crypto.h"
#include "test.h"
#include "modx/util.h"

static void hex_of(const unsigned char *b, size_t n, char *out) { hex_encode(b, n, out); }

static void t_sha256(void) {
    /* FIPS 180-4 / NIST CAVP example vectors */
    struct { const char *msg; const char *hex; } v[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
          "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1" },
    };
    unsigned char h[32];
    char hex[65];
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        sha256(v[i].msg, strlen(v[i].msg), h);
        hex_of(h, 32, hex);
        CHECK_STR(hex, v[i].hex);
    }
    /* one million 'a', fed in uneven chunks to exercise buffering */
    sha256_ctx c;
    sha256_init(&c);
    char chunk[997];
    memset(chunk, 'a', sizeof chunk);
    size_t left = 1000000;
    while (left) {
        size_t n = left < sizeof chunk ? left : sizeof chunk;
        sha256_update(&c, chunk, n);
        left -= n;
    }
    sha256_final(&c, h);
    hex_of(h, 32, hex);
    CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    /* padding boundaries: 55, 56, 63, 64 bytes must equal byte-by-byte hashing */
    for (size_t len = 50; len <= 130; len++) {
        unsigned char *buf = xmalloc(len);
        memset(buf, 'x', len);
        unsigned char a[32], b[32];
        sha256(buf, len, a);
        sha256_init(&c);
        for (size_t i = 0; i < len; i++) sha256_update(&c, buf + i, 1);
        sha256_final(&c, b);
        CHECK(memcmp(a, b, 32) == 0);
        free(buf);
    }
}

static void t_hmac(void) {
    /* RFC 4231 test cases 1, 2, 3, 4, 6, 7 */
    unsigned char key[131], data[200];
    unsigned char out[32];
    char hex[65];

    memset(key, 0x0b, 20);
    hmac_sha256(key, 20, "Hi There", 8, out);
    hex_of(out, 32, hex);
    CHECK_STR(hex, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    hex_of(out, 32, hex);
    CHECK_STR(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    memset(key, 0xaa, 20);
    memset(data, 0xdd, 50);
    hmac_sha256(key, 20, data, 50, out);
    hex_of(out, 32, hex);
    CHECK_STR(hex, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    for (int i = 0; i < 25; i++) key[i] = (unsigned char)(i + 1);
    memset(data, 0xcd, 50);
    hmac_sha256(key, 25, data, 50, out);
    hex_of(out, 32, hex);
    CHECK_STR(hex, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");

    memset(key, 0xaa, 131);
    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha256(key, 131, m6, strlen(m6), out);
    hex_of(out, 32, hex);
    CHECK_STR(hex, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    const char *m7 = "This is a test using a larger than block-size key and a larger than block-size data. "
                     "The key needs to be hashed before being used by the HMAC algorithm.";
    hmac_sha256(key, 131, m7, strlen(m7), out);
    hex_of(out, 32, hex);
    CHECK_STR(hex, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

static void t_pbkdf2(void) {
    unsigned char out[64];
    char hex[129];
    /* RFC 7914 section 11 */
    pbkdf2_sha256("passwd", 6, "salt", 4, 1, out, 64);
    hex_of(out, 64, hex);
    CHECK_STR(hex, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                   "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    pbkdf2_sha256("Password", 8, "NaCl", 4, 80000, out, 64);
    hex_of(out, 64, hex);
    CHECK_STR(hex, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
                   "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");
    /* widely published PBKDF2-HMAC-SHA256 vectors (RFC 6070 inputs) */
    pbkdf2_sha256("password", 8, "salt", 4, 1, out, 32);
    hex_of(out, 32, hex);
    CHECK_STR(hex, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    pbkdf2_sha256("password", 8, "salt", 4, 2, out, 32);
    hex_of(out, 32, hex);
    CHECK_STR(hex, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    pbkdf2_sha256("password", 8, "salt", 4, 4096, out, 32);
    hex_of(out, 32, hex);
    CHECK_STR(hex, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
}

static void t_password(void) {
    char *h = password_hash_iters("correct horse", 1000);
    CHECK(h && strncmp(h, "pbkdf2_sha256$1000$", 19) == 0);
    CHECK(h && strlen(h) == 19 + 32 + 1 + 64);
    CHECK(password_verify("correct horse", h));
    CHECK(!password_verify("correct horsf", h));
    CHECK(!password_verify("", h));
    char *h2 = password_hash_iters("correct horse", 1000);
    CHECK(strcmp(h, h2) != 0); /* random salt */
    free(h2);
    /* tampered / malformed stored strings never verify */
    h[strlen(h) - 1] = h[strlen(h) - 1] == '0' ? '1' : '0';
    CHECK(!password_verify("correct horse", h));
    free(h);
    CHECK(!password_verify("x", NULL));
    CHECK(!password_verify("x", "plain"));
    CHECK(!password_verify("x", "pbkdf2_sha256$abc$00$00"));
    CHECK(!password_verify("x", "pbkdf2_sha256$1$zz$00"));
    /* the default cost is the contract's */
    char *def = password_hash("modx-demo-2026");
    CHECK(def && strncmp(def, "pbkdf2_sha256$150000$", 21) == 0);
    CHECK(password_verify("modx-demo-2026", def));
    free(def);
}

static void t_misc(void) {
    CHECK(ct_equal("abcd", "abcd", 4));
    CHECK(!ct_equal("abcd", "abce", 4));
    unsigned char a[32], b[32];
    CHECK(crypto_random(a, sizeof a) == 0 && crypto_random(b, sizeof b) == 0);
    CHECK(memcmp(a, b, 32) != 0);
    unsigned char dec[4];
    CHECK(hex_decode("00fFa1", dec, sizeof dec) == 3 && dec[1] == 0xff && dec[2] == 0xa1);
    CHECK(hex_decode("abc", dec, sizeof dec) == -1);
    CHECK(hex_decode("zz", dec, sizeof dec) == -1);
    CHECK(hex_decode("0011223344", dec, sizeof dec) == -1);
}

void test_crypto_all(void) {
    RUN(t_sha256);
    RUN(t_hmac);
    RUN(t_pbkdf2);
    RUN(t_password);
    RUN(t_misc);
}
