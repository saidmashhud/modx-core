#ifndef MODX_CRYPTO_H
#define MODX_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN 32
#define SHA256_BLOCK 64

typedef struct {
    uint32_t h[8];
    uint64_t total;          /* bytes processed */
    unsigned char buf[SHA256_BLOCK];
    size_t n;                /* bytes in buf */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, unsigned char out[SHA256_LEN]);
void sha256(const void *data, size_t len, unsigned char out[SHA256_LEN]);

void hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen, unsigned char out[SHA256_LEN]);
void pbkdf2_sha256(const void *pw, size_t pwlen, const void *salt, size_t saltlen, uint32_t iters,
                   unsigned char *out, size_t outlen);

/* Fill buf from the OS CSPRNG. Returns 0, or -1 if no source is available. */
int crypto_random(void *buf, size_t len);

/* Constant-time comparison (time depends only on len). 1 = equal. */
int ct_equal(const void *a, const void *b, size_t len);

/* Lowercase hex; out must hold 2*len+1 bytes. */
void hex_encode(const unsigned char *in, size_t len, char *out);
/* Returns number of bytes decoded, or -1 on invalid input / overflow. */
long hex_decode(const char *in, unsigned char *out, size_t outcap);

#define SHA1_LEN 20
/* SHA-1: only for protocol needs such as the WebSocket handshake (not for secrets). */
void sha1(const void *data, size_t len, unsigned char out[SHA1_LEN]);

/* Standard base64 with padding; out must hold 4 * ((len + 2) / 3) + 1 bytes. */
void base64_encode(const unsigned char *in, size_t len, char *out);
/* Returns bytes decoded, or -1 on invalid input / overflow. */
long base64_decode(const char *in, unsigned char *out, size_t outcap);

#define PASSWORD_ITERS 150000
#define PASSWORD_SALT_LEN 16

/* "pbkdf2_sha256$<iters>$<salt hex>$<hash hex>"; caller frees. NULL if no randomness. */
char *password_hash(const char *password);
char *password_hash_iters(const char *password, uint32_t iters);
/* 1 if password matches the stored string (constant-time digest compare). */
int password_verify(const char *password, const char *stored);

#endif
