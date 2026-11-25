#include <stdlib.h>

#include "modx/session.h"
#include "modx/util.h"
#include "test.h"

static void t_cookie(void) {
    char v[128];
    CHECK(cookie_get("sid=abc", "sid", v, sizeof v) && strcmp(v, "abc") == 0);
    CHECK(cookie_get("a=1; sid=tok; b=2", "sid", v, sizeof v) && strcmp(v, "tok") == 0);
    CHECK(cookie_get("  a=1;sid = tok2 ;b=2", "sid", v, sizeof v) && strcmp(v, "tok2") == 0);
    CHECK(cookie_get("a=1;\tsid=\"quoted\"", "sid", v, sizeof v) && strcmp(v, "quoted") == 0);
    CHECK(cookie_get("x_sid=no; sid=yes", "sid", v, sizeof v) && strcmp(v, "yes") == 0);
    CHECK(cookie_get("sid=first; sid=second", "sid", v, sizeof v) && strcmp(v, "first") == 0);
    CHECK(cookie_get("flag; sid=z;", "sid", v, sizeof v) && strcmp(v, "z") == 0);
    CHECK(cookie_get("sid=", "sid", v, sizeof v) && v[0] == 0);
    CHECK(!cookie_get("sidx=1; other=2", "sid", v, sizeof v));
    CHECK(!cookie_get("", "sid", v, sizeof v));
    CHECK(!cookie_get(NULL, "sid", v, sizeof v));
    CHECK(!cookie_get("sid=toolongvalue", "sid", v, 5)); /* does not fit */
}


static void t_origin_ct(void) {
    CHECK(origin_matches_host("http://localhost:8787", "localhost:8787"));
    CHECK(origin_matches_host("http://LOCALHOST:8787", "localhost:8787"));
    CHECK(origin_matches_host("http://example.com", "example.com"));
    CHECK(origin_matches_host("http://example.com", "example.com:80"));
    CHECK(origin_matches_host("https://example.com", "example.com:443"));
    CHECK(origin_matches_host("http://[::1]:8787", "[::1]:8787"));
    CHECK(!origin_matches_host("https://example.com", "example.com:80"));
    CHECK(!origin_matches_host("http://localhost:5173", "localhost:8787"));
    CHECK(!origin_matches_host("http://evil.com", "localhost:8787"));
    CHECK(!origin_matches_host("null", "localhost:8787"));
    CHECK(!origin_matches_host("http://localhost:8787", NULL));
    CHECK(!origin_matches_host("ftp://localhost:8787", "localhost:8787"));
    CHECK(content_type_is_json("application/json"));
    CHECK(content_type_is_json("Application/JSON; charset=utf-8"));
    CHECK(content_type_is_json("application/json;charset=UTF-8"));
    CHECK(!content_type_is_json("text/plain"));
    CHECK(!content_type_is_json("application/jsonx"));
    CHECK(!content_type_is_json("application/x-www-form-urlencoded"));
    CHECK(!content_type_is_json(NULL));
}


/* 1 when the exact key is stored */
static int rl_find_for_test(const ratelimit *rl, const char *key) {
    for (size_t i = 0; i < rl->n; i++)
        if (strcmp(rl->entries[i].ip, key) == 0) return 1;
    return 0;
}

static void t_ratelimit(void) {
    ratelimit rl;
    rl_init(&rl);
    int64_t t = 1000000;
    for (int i = 0; i < MX_RL_MAX_FAILS; i++) {
        CHECK(!rl_blocked(&rl, "1.2.3.4", t + i));
        rl_fail(&rl, "1.2.3.4", t + i);
    }
    CHECK(rl_blocked(&rl, "1.2.3.4", t + 20));        /* 11th attempt is refused */
    CHECK(!rl_blocked(&rl, "5.6.7.8", t + 20));       /* per IP */
    CHECK(rl_blocked(&rl, "1.2.3.4", t + MX_RL_WINDOW_MS - 1));
    CHECK(!rl_blocked(&rl, "1.2.3.4", t + MX_RL_WINDOW_MS + 1)); /* first failure aged out */
    rl_fail(&rl, "9.9.9.9", t);
    rl_reset(&rl, "1.2.3.4");
    CHECK(!rl_blocked(&rl, "1.2.3.4", t + 30));
    /* a reset forgets the key */
    CHECK(rl.n == 1 && !rl_find_for_test(&rl, "1.2.3.4") && rl_find_for_test(&rl, "9.9.9.9"));
    /* stale entries are pruned when new IPs arrive */
    rl_fail(&rl, "10.0.0.1", t + 3 * MX_RL_WINDOW_MS);
    CHECK(rl.n == 1);
    rl_free(&rl);
}

/* The table is bounded: aged-out keys go first, then the oldest unblocked one. */
static void t_ratelimit_bounded(void) {
    ratelimit rl;
    rl_init_cap(&rl, 64);
    int64_t t = 5000000;
    char ip[32];
    /* one blocked key, then far more distinct keys than fit */
    for (int i = 0; i < MX_RL_MAX_FAILS; i++) rl_fail(&rl, "6.6.6.6", t);
    for (int i = 0; i < 100000; i++) {
        snprintf(ip, sizeof ip, "10.%d.%d.%d", (i >> 16) & 255, (i >> 8) & 255, i & 255);
        rl_fail(&rl, ip, t + 1 + i); /* distinct times: "oldest" is well defined */
    }
    CHECK(rl.n == 64 && rl.cap == 64);
    CHECK(rl.evictions == 100000 - 63);
    const int64_t t2 = t + 200000;
    CHECK(rl_blocked(&rl, "6.6.6.6", t2)); /* unblocked keys were evicted, not the blocked one */
    /* the most recent keys are still there, and the index finds them (no duplicates) */
    int found = 0;
    for (int i = 100000 - 63; i < 100000; i++) {
        snprintf(ip, sizeof ip, "10.%d.%d.%d", (i >> 16) & 255, (i >> 8) & 255, i & 255);
        found += rl_find_for_test(&rl, ip);
        for (int k = 1; k < MX_RL_MAX_FAILS; k++) rl_fail(&rl, ip, t2);
        found += rl_blocked(&rl, ip, t2);
    }
    CHECK(found == 2 * 63 && rl.n == 64 && rl.evictions == 100000 - 63);
    /* full and every key blocked: the oldest blocked one (6.6.6.6) makes room */
    for (int i = 0; i < MX_RL_MAX_FAILS; i++) rl_fail(&rl, "7.7.7.7", t2 + 100);
    CHECK(rl_blocked(&rl, "7.7.7.7", t2 + 100) && rl.n == 64 && !rl_blocked(&rl, "6.6.6.6", t2 + 100));
    /* once the window passed, a new key makes room by dropping every aged-out one (no eviction) */
    int64_t ev = rl.evictions;
    rl_fail(&rl, "8.8.8.8", t2 + 100 + MX_RL_WINDOW_MS);
    CHECK(rl.n < 64 && rl.evictions == ev && !rl_blocked(&rl, "7.7.7.7", t2 + 100 + MX_RL_WINDOW_MS));
    /* and the sweep keeps dropping aged-out keys as new ones arrive */
    for (int i = 0; i < 63; i++) {
        snprintf(ip, sizeof ip, "s%d", i);
        rl_fail(&rl, ip, t2 + 100 + MX_RL_WINDOW_MS);
    }
    CHECK(rl.n == 64 && rl.evictions == ev); /* the 60 stale keys were swept, nothing evicted */
    for (int i = 0; i < 63; i++) {
        snprintf(ip, sizeof ip, "s%d", i);
        rl_reset(&rl, ip);
    }
    rl_reset(&rl, "8.8.8.8");
    CHECK(rl.n == 0);
    /* removals keep the index consistent: reset every other key, the rest are found */
    for (int i = 0; i < 60; i++) {
        snprintf(ip, sizeof ip, "k%d", i);
        rl_fail(&rl, ip, t2 + 200 + MX_RL_WINDOW_MS);
    }
    for (int i = 0; i < 60; i += 2) {
        snprintf(ip, sizeof ip, "k%d", i);
        rl_reset(&rl, ip);
    }
    CHECK(rl.n == 30);
    found = 0;
    for (int i = 1; i < 60; i += 2) {
        snprintf(ip, sizeof ip, "k%d", i);
        for (int k = 1; k < MX_RL_MAX_FAILS; k++) rl_fail(&rl, ip, t2 + 300 + MX_RL_WINDOW_MS);
        found += rl_blocked(&rl, ip, t2 + 300 + MX_RL_WINDOW_MS);
    }
    CHECK(found == 30 && rl.n == 30);
    for (int i = 0; i < 60; i += 2) {
        snprintf(ip, sizeof ip, "k%d", i);
        found += rl_find_for_test(&rl, ip) || rl_blocked(&rl, ip, t2 + 300 + MX_RL_WINDOW_MS);
    }
    CHECK(found == 30);
    /* every key is blocked: the oldest blocked one goes */
    rl_free(&rl);
    rl_init_cap(&rl, 4);
    for (int k = 0; k < 4; k++) {
        snprintf(ip, sizeof ip, "b%d", k);
        for (int i = 0; i < MX_RL_MAX_FAILS; i++) rl_fail(&rl, ip, t + k);
    }
    rl_fail(&rl, "new", t + 10);
    CHECK(rl.n == 4 && !rl_blocked(&rl, "b0", t + 10) && rl_blocked(&rl, "b1", t + 10) && rl.evictions == 1);
    /* long keys are truncated consistently */
    char longkey[200];
    memset(longkey, 'z', sizeof longkey - 1);
    longkey[sizeof longkey - 1] = 0;
    for (int i = 0; i < MX_RL_MAX_FAILS; i++) rl_fail(&rl, longkey, t + 20);
    CHECK(rl_blocked(&rl, longkey, t + 20));
    rl_reset(&rl, longkey);
    CHECK(!rl_blocked(&rl, longkey, t + 20));
    rl_free(&rl);
}


static void t_xff_token(void) {
    char ip[64];
    CHECK(xff_rightmost("1.1.1.1, 10.0.0.2", ip, sizeof ip) && strcmp(ip, "10.0.0.2") == 0);
    CHECK(xff_rightmost(" ::1 ", ip, sizeof ip) && strcmp(ip, "::1") == 0);
    CHECK(!xff_rightmost("1.1.1.1, evil<script>", ip, sizeof ip));
    CHECK(!xff_rightmost(NULL, ip, sizeof ip));
    char tok[65], h[65], h2[65];
    CHECK(session_token_new(tok, h) == 0);
    CHECK(strlen(tok) == 64 && strlen(h) == 64 && strcmp(tok, h) != 0);
    session_token_hash(tok, h2);
    CHECK_STR(h, h2);
    char tok2[65], hh[65];
    CHECK(session_token_new(tok2, hh) == 0 && strcmp(tok, tok2) != 0);
}

static void t_ip_addresses(void) {
    char ip[64];
    CHECK(mx_ip_normalize("203.0.113.7", ip, sizeof ip) && strcmp(ip, "203.0.113.7") == 0);
    CHECK(mx_ip_normalize(" 203.0.113.7:4431 ", ip, sizeof ip) && strcmp(ip, "203.0.113.7") == 0);
    CHECK(mx_ip_normalize("2001:DB8:0:0:0:0:0:7", ip, sizeof ip) && strcmp(ip, "2001:db8::7") == 0);
    CHECK(mx_ip_normalize("[2001:db8::7]:443", ip, sizeof ip) && strcmp(ip, "2001:db8::7") == 0);
    CHECK(mx_ip_normalize("[::1]", ip, sizeof ip) && strcmp(ip, "::1") == 0);
    CHECK(mx_ip_normalize("::ffff:10.0.0.2", ip, sizeof ip) && strcmp(ip, "10.0.0.2") == 0);
    CHECK(mx_ip_normalize("fe80::1%en0", ip, sizeof ip) && strcmp(ip, "fe80::1") == 0);
    CHECK(!mx_ip_normalize("1.2.3", ip, sizeof ip));
    CHECK(!mx_ip_normalize("1.2.3.4:", ip, sizeof ip));
    CHECK(!mx_ip_normalize("1.2.3.4:80x", ip, sizeof ip));
    CHECK(!mx_ip_normalize("[::1", ip, sizeof ip));
    CHECK(!mx_ip_normalize("[::1]x", ip, sizeof ip));
    CHECK(!mx_ip_normalize("example.com", ip, sizeof ip));
    CHECK(!mx_ip_normalize("", ip, sizeof ip) && !mx_ip_normalize(NULL, ip, sizeof ip));
    char tiny[4];
    CHECK(!mx_ip_normalize("10.0.0.1", tiny, sizeof tiny));
    /* limiter keys: IPv4 as is, IPv6 per /64 (or another prefix) */
    CHECK(mx_ip_rate_key("198.51.100.4", 0, ip, sizeof ip) && strcmp(ip, "198.51.100.4") == 0);
    CHECK(mx_ip_rate_key("2001:db8:1:2:aaaa::1", 0, ip, sizeof ip) && strcmp(ip, "2001:db8:1:2::/64") == 0);
    char k2[64];
    CHECK(mx_ip_rate_key("2001:db8:1:2:ffff:1:2:3", 0, k2, sizeof k2) && strcmp(ip, k2) == 0);
    CHECK(mx_ip_rate_key("2001:db8:1:2:ffff:1:2:3", 56, ip, sizeof ip) && strcmp(ip, "2001:db8:1::/56") == 0);
    CHECK(mx_ip_rate_key("2001:db8::1", 128, ip, sizeof ip) && strcmp(ip, "2001:db8::1") == 0);
    CHECK(mx_ip_rate_key("::ffff:192.0.2.1", 0, ip, sizeof ip) && strcmp(ip, "192.0.2.1") == 0);
    CHECK(!mx_ip_rate_key("nope", 0, ip, sizeof ip));
    /* X-Forwarded-For, rightmost element, normalized */
    CHECK(xff_rightmost("198.51.100.1, [2001:DB8::9]:1234", ip, sizeof ip) && strcmp(ip, "2001:db8::9") == 0);
    CHECK(xff_rightmost("::ffff:203.0.113.5", ip, sizeof ip) && strcmp(ip, "203.0.113.5") == 0);
    CHECK(xff_rightmost("203.0.113.5:61000", ip, sizeof ip) && strcmp(ip, "203.0.113.5") == 0);
    CHECK(!xff_rightmost("10.0.0.1, unknown", ip, sizeof ip));
    CHECK(!xff_rightmost("10.0.0.1, ", ip, sizeof ip));
    CHECK(!xff_rightmost("1.2.3", ip, sizeof ip)); /* only hex digits, dots and colons was not enough */
}

void test_session_all(void) {
    RUN(t_cookie);
    RUN(t_origin_ct);
    RUN(t_ratelimit);
    RUN(t_ratelimit_bounded);
    RUN(t_xff_token);
    RUN(t_ip_addresses);
}
