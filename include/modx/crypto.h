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

/* PBKDF2-HMAC-SHA256 iterations for new hashes (OWASP 2023: 600 000). Until
 * 0.2 it was 150 000; such hashes still verify (the count is part of the
 * string), and password_needs_rehash() reports them. */
#define PASSWORD_ITERS 600000
#define PASSWORD_ITERS_LEGACY 150000
#define PASSWORD_SALT_LEN 16

/* "pbkdf2_sha256$<iters>$<salt hex>$<hash hex>"; caller frees. NULL if no randomness.
 * Synchronous: about half a second of CPU at PASSWORD_ITERS. In a server use
 * password_hash_async() so the event loop keeps running. */
char *password_hash(const char *password);
char *password_hash_iters(const char *password, uint32_t iters);
/* 1 if password matches the stored string (constant-time digest compare).
 * Synchronous (as costly as the stored iteration count); see password_verify_async(). */
int password_verify(const char *password, const char *stored);
/* 1 when stored should be replaced by a fresh hash after a successful login:
 * fewer than min_iters iterations (0 = PASSWORD_ITERS) or not a hash this
 * library writes. */
int password_needs_rehash(const char *stored, uint32_t min_iters);

/* Asynchronous hashing on a worker pool (modx/pool.h). The PBKDF2 work runs on
 * a worker thread; the callback runs on the thread that runs the pool's
 * completions (the event loop, with mx_pool_attach). The password is copied
 * (and wiped after use), so the caller's buffer may go away right after the
 * call. Return 0 (the callback will run exactly once) or -1 (not queued: no
 * randomness, or the pool refused with errno EAGAIN when its queue is full;
 * the callback never runs; answer 503 or fall back to the sync call).
 * Use from an HTTP handler with http_defer(): see docs/guides/sessions.md. */
typedef struct mx_pool mx_pool;
/* hash: malloc'ed "pbkdf2_sha256$..." string the callback owns (frees). */
typedef void (*password_hash_cb)(void *ctx, char *hash);
/* ok: 1 if the password matches. needs_rehash: ok and the stored hash is
 * below PASSWORD_ITERS (store a fresh one: password_hash_async). */
typedef void (*password_verify_cb)(void *ctx, int ok, int needs_rehash);
/* iters 0 = PASSWORD_ITERS. */
int password_hash_async(mx_pool *pool, const char *password, uint32_t iters, password_hash_cb cb, void *ctx);
/* A stored string that is not a valid hash fails (ok = 0) without the work, still via the pool. */
int password_verify_async(mx_pool *pool, const char *password, const char *stored, password_verify_cb cb, void *ctx);

/* Overwrites len bytes with zeros in a way the compiler keeps. */
void secure_zero(void *p, size_t len);

#endif
