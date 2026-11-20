#include <math.h>
#include <stdlib.h>

#include "modx/json.h"
#include "test.h"

static char *roundtrip(const char *in) {
    json *v = json_parse(in, strlen(in), NULL, 0);
    if (!v) return NULL;
    char *out = json_dump(v);
    json_free(v);
    return out;
}

static void t_roundtrip(void) {
    const char *doc = "{\"a\":1,\"b\":[true,false,null,\"x\"],\"c\":{\"d\":{\"e\":[[],{}]}},\"f\":-12.5}";
    char *out = roundtrip(doc);
    CHECK_STR(out, doc);
    free(out);

    /* whitespace is dropped, order preserved */
    out = roundtrip(" { \"z\" : 1 ,\n\t\"a\" : [ 1 , 2 ] } ");
    CHECK_STR(out, "{\"z\":1,\"a\":[1,2]}");
    free(out);

    /* duplicate keys: last one wins */
    out = roundtrip("{\"k\":1,\"k\":2}");
    CHECK_STR(out, "{\"k\":2}");
    free(out);
}

static void t_escaping(void) {
    json *s = json_str("quote\" back\\ nl\n tab\t cr\r bell\x01 del\x7f slash/");
    char *out = json_dump(s);
    CHECK_STR(out, "\"quote\\\" back\\\\ nl\\n tab\\t cr\\r bell\\u0001 del\x7f slash/\"");
    json *back = json_parse(out, strlen(out), NULL, 0);
    CHECK(back && json_equal(back, s));
    json_free(back);
    free(out);
    json_free(s);

    /* UTF-8 passes through untouched */
    const char *utf = "\"Привет, мир — 日本 🏗️\"";
    out = roundtrip(utf);
    CHECK_STR(out, utf);
    free(out);

    /* \u escapes decode to UTF-8, including surrogate pairs */
    json *u = json_parse("\"\\u0041\\u00e9\\u041f\\ud83c\\udfd7\\/\"", 34, NULL, 0);
    CHECK(u && u->type == JSON_STRING);
    if (u) CHECK_STR(u->str, "A\xc3\xa9\xd0\x9f\xf0\x9f\x8f\x97/");
    json_free(u);

    /* lone surrogate becomes U+FFFD */
    u = json_parse("\"\\udc00x\"", 9, NULL, 0);
    CHECK(u && strcmp(u->str, "\xef\xbf\xbdx") == 0);
    json_free(u);

    /* embedded NUL survives with explicit length */
    u = json_parse("\"a\\u0000b\"", 10, NULL, 0);
    CHECK(u && u->str_len == 3 && u->str[1] == 0);
    out = json_dump(u);
    CHECK_STR(out, "\"a\\u0000b\"");
    free(out);
    json_free(u);
}

static void t_numbers(void) {
    struct { const char *in, *out; } cases[] = {
        { "0", "0" },
        { "-0", "0" },
        { "42", "42" },
        { "-17", "-17" },
        { "1712345678901", "1712345678901" },
        { "0.1", "0.1" },
        { "3.14159", "3.14159" },
        { "1e300", "1.0000000000000001e+300" },
        { "2.5E-3", "0.0025" },
        { "1024.5", "1024.5" },
        { "9007199254740993", "9007199254740992" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char *out = roundtrip(cases[i].in);
        /* the value must survive a parse->dump->parse cycle exactly */
        json *a = json_parse(cases[i].in, strlen(cases[i].in), NULL, 0);
        json *b = out ? json_parse(out, strlen(out), NULL, 0) : NULL;
        CHECK(a && b && a->number == b->number);
        if (strcmp(cases[i].in, "1e300") != 0) CHECK_STR(out, cases[i].out);
        json_free(a);
        json_free(b);
        free(out);
    }
    json *n = json_num(NAN);
    char *out = json_dump(n);
    CHECK_STR(out, "null");
    free(out);
    json_free(n);
    json *big = json_parse("1e400", 5, NULL, 0);
    CHECK(big == NULL);
}

static void t_invalid(void) {
    const char *bad[] = {
        "", "  ", "{", "}", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "{a:1}", "\"unterminated", "\"ctl\x01\"",
        "01", "1.", ".5", "-", "1e", "+1", "nul", "tru", "[1 2]", "{} {}", "\"\\x\"", "\"\\u12G4\"", "'single'",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char err[128];
        json *v = json_parse(bad[i], strlen(bad[i]), err, sizeof err);
        if (v) fprintf(stderr, "  unexpectedly parsed: %s\n", bad[i]);
        CHECK(v == NULL);
        CHECK(err[0] != 0);
        json_free(v);
    }
    /* depth limit */
    char deep[600];
    memset(deep, '[', 300);
    memset(deep + 300, ']', 300);
    CHECK(json_parse(deep, 600, NULL, 0) == NULL);
    json *ok = json_parse(deep + 100, 400, NULL, 0); /* 200 levels is fine */
    CHECK(ok != NULL);
    json_free(ok);
}

static void t_object_api(void) {
    json *o = json_obj();
    json_set(o, "a", json_num(1));
    json_set(o, "b", json_str("x"));
    json_set(o, "c", json_null());
    json_set(o, "a", json_num(2)); /* replace keeps position */
    char *out = json_dump(o);
    CHECK_STR(out, "{\"a\":2,\"b\":\"x\",\"c\":null}");
    free(out);
    json_del(o, "b");
    out = json_dump(o);
    CHECK_STR(out, "{\"a\":2,\"c\":null}");
    free(out);
    CHECK(json_is_null_or_missing(o, "c"));
    CHECK(json_is_null_or_missing(o, "zzz"));
    CHECK(json_get_num(o, "a", 0) == 2);
    json *c = json_clone(o);
    CHECK(json_equal(o, c));
    json_set(c, "a", json_num(3));
    CHECK(!json_equal(o, c));
    json_free(c);
    json *arr = json_arr();
    json_push(arr, json_bool(1));
    json_set(o, "arr", arr);
    out = json_dump(o);
    CHECK_STR(out, "{\"a\":2,\"c\":null,\"arr\":[true]}");
    free(out);
    json_free(o);
    CHECK(json_is_int(&(json){ .type = JSON_NUMBER, .number = 5 }));
    CHECK(!json_is_int(&(json){ .type = JSON_NUMBER, .number = 5.5 }));
}

void test_json_all(void) {
    RUN(t_roundtrip);
    RUN(t_escaping);
    RUN(t_numbers);
    RUN(t_invalid);
    RUN(t_object_api);
}
