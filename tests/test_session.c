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
    /* stale entries are pruned when new IPs arrive */
    rl_fail(&rl, "10.0.0.1", t + 3 * MX_RL_WINDOW_MS);
    CHECK(rl.n == 1);
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

void test_session_all(void) {
    RUN(t_cookie);
    RUN(t_origin_ct);
    RUN(t_ratelimit);
    RUN(t_xff_token);
}
