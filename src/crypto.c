#include "modx/crypto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "modx/util.h"

#if defined(__linux__)
#include <sys/random.h>
#endif

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void compress(uint32_t h[8], const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(sha256_ctx *c) {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(c->h, iv, sizeof iv);
    c->total = 0;
    c->n = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len) {
    const unsigned char *p = data;
    c->total += len;
    if (c->n) {
        size_t take = SHA256_BLOCK - c->n;
        if (take > len) take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take;
        p += take;
        len -= take;
        if (c->n == SHA256_BLOCK) { compress(c->h, c->buf); c->n = 0; }
    }
    while (len >= SHA256_BLOCK) { compress(c->h, p); p += SHA256_BLOCK; len -= SHA256_BLOCK; }
    if (len) { memcpy(c->buf, p, len); c->n = len; }
}

void sha256_final(sha256_ctx *c, unsigned char out[SHA256_LEN]) {
    uint64_t bits = c->total * 8;
    c->buf[c->n++] = 0x80;
    if (c->n > 56) {
        memset(c->buf + c->n, 0, SHA256_BLOCK - c->n);
        compress(c->h, c->buf);
        c->n = 0;
    }
    memset(c->buf + c->n, 0, 56 - c->n);
    for (int i = 0; i < 8; i++) c->buf[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
    compress(c->h, c->buf);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(c->h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(c->h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(c->h[i] >> 8);
        out[4 * i + 3] = (unsigned char)c->h[i];
    }
    memset(c, 0, sizeof *c);
}

void sha256(const void *data, size_t len, unsigned char out[SHA256_LEN]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* Inner/outer contexts after absorbing the padded key. */
static void hmac_setup(const void *key, size_t keylen, sha256_ctx *inner, sha256_ctx *outer) {
    unsigned char k[SHA256_BLOCK];
    memset(k, 0, sizeof k);
    if (keylen > SHA256_BLOCK) sha256(key, keylen, k);
    else if (keylen) memcpy(k, key, keylen);
    unsigned char ipad[SHA256_BLOCK], opad[SHA256_BLOCK];
    for (int i = 0; i < SHA256_BLOCK; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    sha256_init(inner);
    sha256_update(inner, ipad, SHA256_BLOCK);
    sha256_init(outer);
    sha256_update(outer, opad, SHA256_BLOCK);
    memset(k, 0, sizeof k);
}

static void hmac_finish(const sha256_ctx *inner0, const sha256_ctx *outer0, const void *msg, size_t msglen,
                        unsigned char out[SHA256_LEN]) {
    sha256_ctx c = *inner0;
    unsigned char ih[SHA256_LEN];
    sha256_update(&c, msg, msglen);
    sha256_final(&c, ih);
    c = *outer0;
    sha256_update(&c, ih, SHA256_LEN);
    sha256_final(&c, out);
}

void hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen, unsigned char out[SHA256_LEN]) {
    sha256_ctx in, ou;
    hmac_setup(key, keylen, &in, &ou);
    hmac_finish(&in, &ou, msg, msglen, out);
}

void pbkdf2_sha256(const void *pw, size_t pwlen, const void *salt, size_t saltlen, uint32_t iters,
                   unsigned char *out, size_t outlen) {
    sha256_ctx in, ou;
    hmac_setup(pw, pwlen, &in, &ou);
    unsigned char *sb = xmalloc(saltlen + 4);
    if (saltlen) memcpy(sb, salt, saltlen);
    for (uint32_t block = 1; outlen; block++) {
        sb[saltlen] = (unsigned char)(block >> 24);
        sb[saltlen + 1] = (unsigned char)(block >> 16);
        sb[saltlen + 2] = (unsigned char)(block >> 8);
        sb[saltlen + 3] = (unsigned char)block;
        unsigned char u[SHA256_LEN], t[SHA256_LEN];
        hmac_finish(&in, &ou, sb, saltlen + 4, u);
        memcpy(t, u, SHA256_LEN);
        for (uint32_t i = 1; i < iters; i++) {
            hmac_finish(&in, &ou, u, SHA256_LEN, u);
            for (int j = 0; j < SHA256_LEN; j++) t[j] ^= u[j];
        }
        size_t take = outlen < SHA256_LEN ? outlen : SHA256_LEN;
        memcpy(out, t, take);
        out += take;
        outlen -= take;
    }
    free(sb);
}

int crypto_random(void *buf, size_t len) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(buf, len);
    return 0;
#else
    unsigned char *p = buf;
    size_t got = 0;
#if defined(__linux__)
    while (got < len) {
        ssize_t n = getrandom(p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            break; /* ENOSYS etc: fall back to /dev/urandom */
        }
        got += (size_t)n;
    }
    if (got == len) return 0;
#endif
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (n == 0) { close(fd); return -1; }
        got += (size_t)n;
    }
    close(fd);
    return 0;
#endif
}

int ct_equal(const void *a, const void *b, size_t len) {
    const volatile unsigned char *x = a, *y = b;
    unsigned char d = 0;
    for (size_t i = 0; i < len; i++) d |= x[i] ^ y[i];
    return d == 0;
}

void hex_encode(const unsigned char *in, size_t len, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) { out[2 * i] = hx[in[i] >> 4]; out[2 * i + 1] = hx[in[i] & 15]; }
    out[2 * len] = 0;
}

static int hexdig(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

long hex_decode(const char *in, unsigned char *out, size_t outcap) {
    size_t n = strlen(in);
    if (n % 2 || n / 2 > outcap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int h = hexdig(in[2 * i]), l = hexdig(in[2 * i + 1]);
        if (h < 0 || l < 0) return -1;
        out[i] = (unsigned char)(h << 4 | l);
    }
    return (long)(n / 2);
}

char *password_hash_iters(const char *password, uint32_t iters) {
    unsigned char salt[PASSWORD_SALT_LEN], dk[SHA256_LEN];
    if (crypto_random(salt, sizeof salt) != 0) return NULL;
    pbkdf2_sha256(password, strlen(password), salt, sizeof salt, iters, dk, sizeof dk);
    char sh[2 * PASSWORD_SALT_LEN + 1], dh[2 * SHA256_LEN + 1];
    hex_encode(salt, sizeof salt, sh);
    hex_encode(dk, sizeof dk, dh);
    char *out = xmalloc(32 + sizeof sh + sizeof dh);
    snprintf(out, 32 + sizeof sh + sizeof dh, "pbkdf2_sha256$%u$%s$%s", iters, sh, dh);
    return out;
}

char *password_hash(const char *password) { return password_hash_iters(password, PASSWORD_ITERS); }

int password_verify(const char *password, const char *stored) {
    if (!stored || strncmp(stored, "pbkdf2_sha256$", 14) != 0) return 0;
    const char *p = stored + 14;
    char *end;
    errno = 0;
    unsigned long iters = strtoul(p, &end, 10);
    if (errno || end == p || *end != '$' || iters < 1 || iters > 10000000) return 0;
    const char *salt_hex = end + 1;
    const char *dollar = strchr(salt_hex, '$');
    if (!dollar || dollar - salt_hex > 2 * 64) return 0;
    char sh[129];
    memcpy(sh, salt_hex, (size_t)(dollar - salt_hex));
    sh[dollar - salt_hex] = 0;
    unsigned char salt[64], want[SHA256_LEN], got[SHA256_LEN];
    long sl = hex_decode(sh, salt, sizeof salt);
    if (sl < 1 || hex_decode(dollar + 1, want, sizeof want) != SHA256_LEN) return 0;
    pbkdf2_sha256(password, strlen(password), salt, (size_t)sl, (uint32_t)iters, got, sizeof got);
    return ct_equal(got, want, SHA256_LEN);
}

/* ---------------- SHA-1 (only for the WebSocket handshake, RFC 6455) ---------------- */

static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void sha1(const void *data, size_t len, unsigned char out[SHA1_LEN]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    const unsigned char *p = data;
    uint64_t bits = (uint64_t)len * 8;
    size_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char block[64];
    for (size_t off = 0; off < total; off += 64) {
        for (size_t i = 0; i < 64; i++) {
            size_t k = off + i;
            if (k < len) block[i] = p[k];
            else if (k == len) block[i] = 0x80;
            else if (k >= total - 8) block[i] = (unsigned char)(bits >> (8 * (total - 1 - k)));
            else block[i] = 0;
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 | (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (unsigned char)(h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(h[i] >> 8);
        out[4 * i + 3] = (unsigned char)h[i];
    }
}

/* ---------------- base64 ---------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64_encode(const unsigned char *in, size_t len, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

long base64_decode(const char *in, unsigned char *out, size_t outcap) {
    size_t n = strlen(in), o = 0;
    if (n % 4) return -1;
    for (size_t i = 0; i < n; i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            char ch = in[i + k];
            const char *pos = ch ? strchr(B64, ch) : NULL;
            v[k] = ch == '=' ? -2 : pos ? (int)(pos - B64) : -1;
            if (v[k] == -1) return -1;
        }
        if (v[0] < 0 || v[1] < 0 || (v[2] == -2 && v[3] != -2) || ((v[2] == -2 || v[3] == -2) && i + 4 != n)) return -1;
        uint32_t x = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)(v[2] < 0 ? 0 : v[2]) << 6 | (uint32_t)(v[3] < 0 ? 0 : v[3]);
        size_t emit = v[2] == -2 ? 1 : v[3] == -2 ? 2 : 3;
        if (o + emit > outcap) return -1;
        out[o++] = (unsigned char)(x >> 16);
        if (emit > 1) out[o++] = (unsigned char)(x >> 8);
        if (emit > 2) out[o++] = (unsigned char)x;
    }
    return (long)o;
}
