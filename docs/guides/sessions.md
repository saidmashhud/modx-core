# Sessions and passwords

Cookie sessions from the pieces in `modx/session.h` and `modx/crypto.h`: cookies, CSRF checks, client
addresses (IPv4 / IPv6, behind a proxy), failure throttling, session tokens, and password hashing that does
not block the event loop.

## Session tokens

Store the hash of a token, never the token:

```c
char token[65], token_hash[65];
if (session_token_new(token, token_hash) != 0) { /* no randomness: 500 */ }
json *s = json_obj();
json_set(s, "id", json_str(token_hash));          /* the row's id is the hash */
json_set(s, "account_id", json_str(account_id));
mx_store_put(st, C_SESSIONS, s, NULL);            /* a MX_COLL_PRIVATE collection */
sbuf_printf(&resp->headers, "Set-Cookie: sid=%s; Path=/; HttpOnly; SameSite=Lax; Secure\r\n", token);
```

On each request: `cookie_get(http_req_header(req, "cookie"), "sid", tok, sizeof tok)`, then
`session_token_hash(tok, h)` and `mx_store_get(st, C_SESSIONS, h)`. A leaked snapshot or journal then holds
no usable tokens.

## CSRF and content type

For cookie-authenticated state changes, require `origin_matches_host(http_req_header(req, "origin"),
http_req_header(req, "host"))` (the Origin's host:port equals Host; default ports implied by the scheme)
and `content_type_is_json(http_req_header(req, "content-type"))`: a cross-site form cannot send
`application/json` without a CORS preflight.

## Client addresses

`http_conn.ip` is the socket peer, IPv4 or IPv6; on a dual-stack listener an IPv4 client shows as
`203.0.113.7`, not `::ffff:203.0.113.7`. Behind a reverse proxy you trust, use the address it appended:

```c
char ip[64];
if (!(trust_proxy && xff_rightmost(http_req_header_last(req, "x-forwarded-for"), ip, sizeof ip)))
    snprintf(ip, sizeof ip, "%s", c->ip);
```

`xff_rightmost()` takes the rightmost element and normalizes it with `mx_ip_normalize()` (canonical text;
accepts `[v6]:port`, `v4:port`, zones, IPv4-mapped); anything that is not an address literal returns 0. Never
take the leftmost element: the client controls it.

## Failure throttling

```c
ratelimit rl;
rl_init(&rl);                                       /* MX_RL_CAPACITY keys; rl_init_cap for another bound */
char key[80];
mx_ip_rate_key(ip, 0, key, sizeof key);             /* IPv4 as is; IPv6 per /64 */
if (rl_blocked(&rl, key, now_ms())) { /* 429 */ }
/* after a failed login: */ rl_fail(&rl, key, now_ms());
/* after a successful one: */ rl_reset(&rl, key);
```

A key is blocked after `MX_RL_MAX_FAILS` (10) failures inside `MX_RL_WINDOW_MS` (10 minutes). Use
`mx_ip_rate_key()` rather than the raw address: one IPv6 subscriber usually controls a whole /64, and
per-address keys would let it rotate through 2^64 addresses. The table is bounded (aged-out keys are swept,
then the oldest unblocked key is evicted), so a flood of addresses cannot grow memory.

## Password hashes

`password_hash()` writes `pbkdf2_sha256$<iterations>$<salt hex>$<hash hex>` with a 16-byte random salt and
`PASSWORD_ITERS` = 600 000 iterations of PBKDF2-HMAC-SHA256 (OWASP's recommendation). The iteration count is
part of the string, so `password_verify()` checks hashes of any count, including the 150 000-iteration ones
(`PASSWORD_ITERS_LEGACY`) that 0.1 and 0.2 wrote. The digest comparison is constant-time.

At 600 000 iterations one hash or verification costs about half a second of CPU. On the loop thread that
would stall every connection for that long, so a server uses the asynchronous versions on a worker pool.
The synchronous ones remain for tools, migrations and tests.

### Asynchronous hashing

```c
mx_pool *pool = mx_pool_new(2, 0);        /* 2 workers, default queue bound (1024) */
mx_pool_attach(pool, &srv);               /* completions run on the loop thread */
...
http_server_close(&srv);
mx_pool_free(pool);                       /* finishes queued jobs, runs their callbacks */
```

`password_verify_async(pool, password, stored, cb, ctx)` and `password_hash_async(pool, password, iters, cb,
ctx)` copy the password (it is wiped after use), prepare the salt and the expected digest on the calling
thread, compute PBKDF2 on a worker, and call `cb` on the loop thread. They return 0 (the callback will run
exactly once) or -1 (not queued: the pool's queue is full, `errno = EAGAIN`; or no randomness), in which case
the callback never runs: answer 503. A stored string that is not a valid hash fails (`ok = 0`) without the
work, still through the callback.

### A login handler

The handler defers its response (see [HTTP: deferred responses](http.md#deferred-responses)) and returns at
once; the loop keeps serving while the worker hashes.

```c
typedef struct { app *a; http_deferred *d; char account[64]; char ip[80]; char *password, *stored; } login;
typedef struct { app *a; char account[64]; char *old_hash; } rehash;
static void rehashed(void *ctx, char *hash);

static void login_checked(void *ctx, int ok, int needs_rehash);

static void auth_login(app *a, http_conn *c, const http_request *req, http_response *resp,
                       const char *email, const char *password, const char *ip) {
    if (rl_blocked(&a->login_rl, ip, now_ms())) { resp->status = 429; return; }
    const json *acct = account_by_email(a, email);
    /* unknown email: verify against a dummy hash, so timing does not reveal which emails exist */
    const char *stored = acct ? json_get_str(acct, "password_hash") : a->dummy_hash;
    login *l = xcalloc(1, sizeof *l);
    l->a = a;
    snprintf(l->account, sizeof l->account, "%s", acct ? json_get_str(acct, "id") : "");
    snprintf(l->ip, sizeof l->ip, "%s", ip);
    l->password = xstrdup(password);                  /* only for a rehash after a match */
    l->stored = xstrdup(stored);
    l->d = http_defer(c, req, resp);
    if (password_verify_async(a->pool, password, stored, login_checked, l) != 0) {
        http_deferred_response(l->d)->status = 503;
        http_deferred_finish(l->d);
        login_free(l);
    }
}

static void login_checked(void *ctx, int ok, int needs_rehash) {
    login *l = ctx;
    http_response *r = http_deferred_response(l->d);
    ok = ok && l->account[0];
    if (!ok) {
        rl_fail(&l->a->login_rl, l->ip, now_ms());
        r->status = 401;
    } else {
        rl_reset(&l->a->login_rl, l->ip);
        create_session(l->a, l->account, r);          /* mx_store_put + Set-Cookie */
        if (needs_rehash) {                            /* see below */
            rehash *rh = xcalloc(1, sizeof *rh);
            rh->a = l->a;
            snprintf(rh->account, sizeof rh->account, "%s", l->account);
            rh->old_hash = xstrdup(l->stored);
            if (password_hash_async(l->a->pool, l->password, 0, rehashed, rh) != 0) { free(rh->old_hash); free(rh); }
        }
    }
    http_deferred_finish(l->d);   /* held until the session row is durable */
    login_free(l);                /* secure_zero + free password, free stored, free l */
}
```

The account may have changed while the worker ran (the loop kept going): look it up again by id in the
callback before acting on it, as `create_session` should.

### Rehash on login

`needs_rehash` is 1 when the password matched and the stored hash has fewer than `PASSWORD_ITERS`
iterations. The password is only known during the login, so that is the moment to upgrade the hash
(`login_checked` above queues it); it must not delay the answer, and it does not: the new hash is stored
when its own job completes.

```c
static void rehashed(void *ctx, char *hash) {        /* loop thread */
    rehash *rh = ctx;
    const json *cur = mx_store_get(rh->a->st, C_ACCOUNTS, rh->account);
    const char *now = cur ? json_get_str(cur, "password_hash") : NULL;
    /* only if the password was not changed meanwhile: never overwrite a newer hash */
    if (hash && now && strcmp(now, rh->old_hash) == 0) {
        json *acct = json_clone(cur);
        json_set(acct, "password_hash", json_str(hash));
        mx_store_put(rh->a->st, C_ACCOUNTS, acct, NULL);
    }
    free(hash);
    free(rh->old_hash);
    free(rh);
}
```

The login context's copy of the password is wiped with `secure_zero()` before it is freed. `password_needs_rehash(stored, 0)` answers the same question without a login (e.g. to count how many
accounts still have old hashes).

### Pool and fork rules

The pool's workers only compute: no store access, no stdio, no malloc in the hashing path. The store's
background compaction forks from the loop thread while workers may be mid-hash; the child serializes and
writes the snapshot and never touches the pool (and `mx_pool_submit()` refuses to run in a forked child).
`make test-tsan` checks the pool, the async hashing and the deferred responses under ThreadSanitizer.

See also: [docs index](../README.md) · [API reference](../reference.md) · [HTTP and routing](http.md) ·
[Store and durability](store.md) · [Idempotency keys](idempotency.md)
