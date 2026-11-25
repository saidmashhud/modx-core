#ifndef MODX_CRYPTO_H
#define MODX_CRYPTO_H

/** @file crypto.h
 * @brief SHA-256, HMAC, PBKDF2, password hashing (sync and on a worker
 * pool), OS randomness, constant-time compare, hex and base64.
 */

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN 32   /**< SHA-256 digest bytes */
#define SHA256_BLOCK 64 /**< SHA-256 block bytes */

/** Incremental SHA-256 state (sha256_init(), sha256_update(), sha256_final()). */
typedef struct {
    uint32_t h[8];           /**< chaining value */
    uint64_t total;          /**< bytes processed */
    unsigned char buf[SHA256_BLOCK]; /**< partial block */
    size_t n;                /**< bytes in buf */
} sha256_ctx;

/** Starts a new digest in @p c. */
void sha256_init(sha256_ctx *c);
/** Feeds @p len bytes. */
void sha256_update(sha256_ctx *c, const void *data, size_t len);
/** Writes the digest to @p out; @p c must be re-initialized before reuse. */
void sha256_final(sha256_ctx *c, unsigned char out[SHA256_LEN]);
/** One-shot SHA-256 of @p len bytes. */
void sha256(const void *data, size_t len, unsigned char out[SHA256_LEN]);

/** HMAC-SHA256 (RFC 2104) of @p msg under @p key. */
void hmac_sha256(const void *key, size_t keylen, const void *msg, size_t msglen, unsigned char out[SHA256_LEN]);
/** PBKDF2-HMAC-SHA256 (RFC 8018): derives @p outlen bytes into @p out. */
void pbkdf2_sha256(const void *pw, size_t pwlen, const void *salt, size_t saltlen, uint32_t iters,
                   unsigned char *out, size_t outlen);

/** Fill @p buf from the OS CSPRNG (arc4random_buf, getrandom or
 * /dev/urandom). @return 0, or -1 if no source is available. */
int crypto_random(void *buf, size_t len);

/** Constant-time comparison (time depends only on @p len). @return 1 = equal. */
int ct_equal(const void *a, const void *b, size_t len);

/** Lowercase hex; @p out must hold 2*len+1 bytes (NUL-terminated). */
void hex_encode(const unsigned char *in, size_t len, char *out);
/** Decodes NUL-terminated hex (either case) into at most @p outcap bytes.
 * @return number of bytes decoded, or -1 on invalid input / odd length /
 *         overflow. */
long hex_decode(const char *in, unsigned char *out, size_t outcap);

#define SHA1_LEN 20 /**< SHA-1 digest bytes */
/** SHA-1: only for protocol needs such as the WebSocket handshake (not for secrets). */
void sha1(const void *data, size_t len, unsigned char out[SHA1_LEN]);

/** Standard base64 with padding; @p out must hold 4 * ((len + 2) / 3) + 1
 * bytes (NUL-terminated). */
void base64_encode(const unsigned char *in, size_t len, char *out);
/** Decodes NUL-terminated padded base64 into at most @p outcap bytes.
 * @return bytes decoded, or -1 on invalid input / overflow. */
long base64_decode(const char *in, unsigned char *out, size_t outcap);

/** PBKDF2-HMAC-SHA256 iterations for new hashes (OWASP 2023: 600 000). Until
 * 0.2 it was PASSWORD_ITERS_LEGACY (150 000); such hashes still verify (the
 * count is part of the string), and password_needs_rehash() reports them. */
#define PASSWORD_ITERS 600000
#define PASSWORD_ITERS_LEGACY 150000 /**< iteration count of the hashes 0.1 and 0.2 wrote */
#define PASSWORD_SALT_LEN 16         /**< salt bytes of new hashes */

/** New hash of @p password at PASSWORD_ITERS:
 * `"pbkdf2_sha256$<iters>$<salt hex>$<hash hex>"`; caller frees. NULL if no
 * randomness. Synchronous: about half a second of CPU at PASSWORD_ITERS. In
 * a server use password_hash_async() so the event loop keeps running. */
char *password_hash(const char *password);
/** password_hash() with an explicit iteration count. */
char *password_hash_iters(const char *password, uint32_t iters);
/** 1 if @p password matches the @p stored string (constant-time digest
 * compare); 0 on mismatch or when stored is not a valid hash. Synchronous
 * (as costly as the stored iteration count); see password_verify_async(). */
int password_verify(const char *password, const char *stored);
/** 1 when @p stored should be replaced by a fresh hash after a successful
 * login: fewer than @p min_iters iterations (0 = PASSWORD_ITERS) or not a
 * hash this library writes. */
int password_needs_rehash(const char *stored, uint32_t min_iters);

/* ---- asynchronous hashing on a worker pool ---- */

/** Worker pool (modx/pool.h). */
typedef struct mx_pool mx_pool;
/** Completion of password_hash_async(). @p hash: malloc'ed
 * `"pbkdf2_sha256$..."` string the callback owns (frees). */
typedef void (*password_hash_cb)(void *ctx, char *hash);
/** Completion of password_verify_async(). @p ok: 1 if the password matches.
 * @p needs_rehash: ok and the stored hash is below PASSWORD_ITERS (store a
 * fresh one: password_hash_async()). */
typedef void (*password_verify_cb)(void *ctx, int ok, int needs_rehash);
/** Hashes @p password on a worker of @p pool (modx/pool.h).
 *
 * The PBKDF2 work runs on a worker thread; @p cb runs on the thread that runs
 * the pool's completions (the event loop, with mx_pool_attach()). The
 * password is copied (and wiped after use), so the caller's buffer may go
 * away right after the call. Use from an HTTP handler with http_defer(): see
 * docs/guides/sessions.md.
 * @param iters 0 = PASSWORD_ITERS
 * @return 0 (the callback will run exactly once) or -1 (not queued: no
 *         randomness (errno EIO), or the pool refused, e.g. errno EAGAIN when
 *         its queue is full; the callback never runs; answer 503 or fall back
 *         to the sync call). */
int password_hash_async(mx_pool *pool, const char *password, uint32_t iters, password_hash_cb cb, void *ctx);
/** Verifies @p password against @p stored on a worker of @p pool; same
 * threading, copying and return rules as password_hash_async(). A stored
 * string that is not a valid hash fails (ok = 0) without the work, still via
 * the pool. */
int password_verify_async(mx_pool *pool, const char *password, const char *stored, password_verify_cb cb, void *ctx);

/** Overwrites @p len bytes with zeros in a way the compiler keeps (volatile
 * stores), for wiping secrets. */
void secure_zero(void *p, size_t len);

#endif
