#ifndef MODX_SESSION_H
#define MODX_SESSION_H

#include <stddef.h>
#include <stdint.h>

/* Reusable pieces of cookie sessions: cookie parsing, CSRF origin checks,
 * proxy-aware client addresses, failure throttling and session tokens.
 * Where sessions are stored is up to the application (usually a private
 * store collection keyed by the token hash). */

#define MX_RL_MAX_FAILS 10                 /* failures per key inside the window */
#define MX_RL_WINDOW_MS (10LL * 60 * 1000) /* sliding window */

/* Find cookie `name` in a Cookie header ("a=1; b=2", any whitespace).
 * Copies the value into out (NUL-terminated). Returns 1 if found and it fit. */
int cookie_get(const char *header, const char *name, char *out, size_t outcap);

/* CSRF: 1 if the Origin header's host:port equals the Host header
 * (default ports 80/443 implied by the origin's scheme). */
int origin_matches_host(const char *origin, const char *host);

/* 1 for "application/json" with optional parameters, case-insensitive. */
int content_type_is_json(const char *ct);

/* Behind a trusted reverse proxy: the rightmost X-Forwarded-For element (the
 * address the proxy appended), trimmed. Only [0-9A-Fa-f.:] characters are
 * accepted (IPv4 / IPv6 literals). Returns 1 and fills out, or 0 (caller falls
 * back to the socket peer). */
int xff_rightmost(const char *header, char *out, size_t outcap);

/* Failure throttling: per-key (usually per-IP) timestamps of recent failures.
 * Bounded: at most `cap` keys (MX_RL_CAPACITY unless rl_init_cap says
 * otherwise; about 160 bytes each, allocated on the first failure), found
 * through a hash index. When it is full, keys whose failures have all aged
 * out are dropped first; if none has, the key with the oldest last failure
 * is evicted, preferring keys that are not blocked. */
#define MX_RL_CAPACITY 4096

typedef struct {
    char ip[64];                    /* the key (truncated to 63 bytes) */
    int64_t fails[MX_RL_MAX_FAILS]; /* ring of the last N failure times */
    int count;                      /* valid entries (<= MX_RL_MAX_FAILS) */
    int next;
    int64_t last;                   /* latest failure */
} rl_entry;

typedef struct {
    rl_entry *entries;  /* n used of cap (dense) */
    size_t n, cap;      /* cap: most keys kept (0 until the first failure, then fixed) */
    uint32_t *index;    /* open addressing, entry position + 1 (0 = empty) */
    size_t index_cap;   /* power of two, >= 2 * cap */
    size_t max;         /* capacity asked for (0 = MX_RL_CAPACITY) */
    int64_t evictions;  /* keys dropped while still inside the window to make room */
} ratelimit;

void rl_init(ratelimit *rl);
/* Like rl_init with a capacity other than MX_RL_CAPACITY (keys). */
void rl_init_cap(ratelimit *rl, size_t max_keys);
void rl_free(ratelimit *rl);
/* 1 when key already has MX_RL_MAX_FAILS failures inside the window. */
int rl_blocked(ratelimit *rl, const char *key, int64_t now);
void rl_fail(ratelimit *rl, const char *key, int64_t now);
/* Forgets key (e.g. after a successful login). */
void rl_reset(ratelimit *rl, const char *key);

/* A new random session token (64 lowercase hex chars) and the hex SHA-256 of
 * it, which is what should be stored. Returns 0, or -1 without randomness. */
int session_token_new(char token[65], char token_hash[65]);
/* Hex SHA-256 of a presented token, for the lookup. */
void session_token_hash(const char *token, char token_hash[65]);

#endif
