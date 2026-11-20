/* OT core: normalization, apply, UTF-16 counting with surrogate pairs,
 * transform vectors and TP1 convergence on randomized ops over astral text. */
#include <stdlib.h>

#include "modx/json.h"
#include "modx/ot.h"
#include "test.h"

static json *J11(const char *txt) {
    json *o = json_parse(txt, strlen(txt), NULL, 0);
    if (!o) { fprintf(stderr, "bad fixture: %s\n", txt); exit(2); }
    return o;
}


/* ---------------------------------------------------------------- OT helpers */

static int parse_op(const char *txt, ot_op *op) {
    json *j = J11(txt);
    char err[160];
    int rc = ot_from_json(j, OT_MAX_ITEMS, op, err, sizeof err);
    json_free(j);
    return rc;
}

static char *op_str(const ot_op *op) {
    json *j = ot_to_json(op);
    char *s = json_dump(j);
    json_free(j);
    return s;
}

/* normalized form of a wire op, or "ERR" */
static char *norm(const char *txt) {
    ot_op op;
    if (parse_op(txt, &op) != 0) return xstrdup("ERR");
    char *s = op_str(&op);
    ot_free(&op);
    return s;
}

#define CHECK_NORM(in, want) do { char *s_ = norm(in); CHECK_STR(s_, want); free(s_); } while (0)

/* apply(text, op) or "ERR" */
static char *app(const char *text, const ot_op *op) {
    sbuf out;
    sbuf_init(&out);
    char err[160];
    if (ot_apply(text, strlen(text), op, &out, err, sizeof err) != 0) { sbuf_free(&out); return xstrdup("ERR"); }
    return sbuf_take(&out);
}

static char *app_s(const char *text, const char *optxt) {
    ot_op op;
    if (parse_op(optxt, &op) != 0) return xstrdup("ERR");
    char *r = app(text, &op);
    ot_free(&op);
    return r;
}

#define CHECK_APPLY(text, op, want) do { char *s_ = app_s(text, op); CHECK_STR(s_, want); free(s_); } while (0)

static void t_normalize_apply(void) {
    /* client vectors (normalize / apply) */
    CHECK_NORM("[1,2,\"a\",\"b\",{\"d\":1},{\"d\":2}]", "[3,\"ab\",{\"d\":3}]");
    CHECK_NORM("[{\"d\":2},\"x\"]", "[\"x\",{\"d\":2}]");
    CHECK_NORM("[\"a\",{\"d\":1},\"b\",{\"d\":1}]", "[\"ab\",{\"d\":2}]");
    CHECK_NORM("[]", "[]");
    /* the server is strict where the client drops empties */
    CHECK_NORM("[0]", "ERR");
    CHECK_NORM("[-1]", "ERR");
    CHECK_NORM("[1.5]", "ERR");
    CHECK_NORM("[\"\"]", "ERR");
    CHECK_NORM("[{\"d\":0}]", "ERR");
    CHECK_NORM("[{\"x\":1}]", "ERR");
    CHECK_NORM("[{\"d\":1,\"x\":1}]", "ERR");
    CHECK_NORM("[[1]]", "ERR");
    CHECK_NORM("[null]", "ERR");
    CHECK_NORM("[true]", "ERR");
    CHECK_NORM("{\"d\":1}", "ERR");
    CHECK_NORM("[\"a\\u0000b\"]", "ERR");
    {
        /* max items */
        sbuf b;
        sbuf_init(&b);
        sbuf_putc(&b, '[');
        for (int i = 0; i <= OT_MAX_ITEMS; i++) sbuf_puts(&b, i ? ",\"x\"" : "\"x\"");
        sbuf_putc(&b, ']');
        CHECK_NORM(b.data, "ERR");
        b.len -= 5; /* drop one item: exactly 10 000 */
        b.data[b.len] = 0;
        sbuf_putc(&b, ']');
        ot_op op;
        CHECK(parse_op(b.data, &op) == 0 && op.len == 1 && op.target_len == OT_MAX_ITEMS);
        ot_free(&op);
        sbuf_free(&b);
    }
    ot_op op;
    CHECK(parse_op("[2,\"abc\",{\"d\":3}]", &op) == 0 && op.base_len == 5 && op.target_len == 5);
    ot_free(&op);
    CHECK_APPLY("hello world", "[6,{\"d\":5},\"there\"]", "hello there");
    CHECK_APPLY("", "[\"abc\"]", "abc");
    CHECK_APPLY("", "[]", "");
    CHECK_APPLY("abc", "[2]", "ERR");
    CHECK_APPLY("abc", "[4]", "ERR");
    CHECK_APPLY("abc", "[]", "ERR");
    CHECK(parse_op("[3]", &op) == 0 && ot_is_noop(&op));
    ot_free(&op);
    CHECK(parse_op("[1,\"x\",2]", &op) == 0 && !ot_is_noop(&op));
    ot_free(&op);
}

static void t_utf16(void) {
    CHECK(ot_utf16_len("", 0) == 0);
    CHECK(ot_utf16_len("abc", 3) == 3);
    CHECK(ot_utf16_len("\xc3\xa9", 2) == 1);                 /* é: one unit */
    CHECK(ot_utf16_len("\xe2\x80\x94", 3) == 1);             /* em dash */
    CHECK(ot_utf16_len("\xf0\x9f\x98\x80", 4) == 2);         /* 😀: a surrogate pair */
    CHECK(ot_utf16_len("a\xf0\x9f\x98\x80" "b", 6) == 4);
    CHECK(ot_utf16_len("\xc3", 1) == -1);                    /* truncated */
    CHECK(ot_utf16_len("\xc0\xaf", 2) == -1);                /* overlong */
    CHECK(ot_utf16_len("\xed\xa0\x80", 3) == -1);            /* a surrogate code point in UTF-8 */
    CHECK(ot_utf16_len("\xf4\x90\x80\x80", 4) == -1);        /* above U+10FFFF */
    CHECK(ot_utf16_len("\x80", 1) == -1);
    /* client vector: the emoji is two code units */
    CHECK_APPLY("a\xf0\x9f\x98\x80" "b", "[1,{\"d\":2},1]", "ab");
    CHECK_APPLY("a\xf0\x9f\x98\x80" "b", "[3,\"x\",1]", "a\xf0\x9f\x98\x80xb");
    /* boundaries inside the pair are refused */
    CHECK_APPLY("a\xf0\x9f\x98\x80" "b", "[2,\"x\",2]", "ERR");
    CHECK_APPLY("a\xf0\x9f\x98\x80" "b", "[1,{\"d\":1},2]", "ERR");
    CHECK_APPLY("a\xf0\x9f\x98\x80" "b", "[2,{\"d\":2}]", "ERR");
    CHECK_APPLY("\xf0\x9f\x98\x80", "[1,{\"d\":1}]", "ERR");
    CHECK_APPLY("\xf0\x9f\x98\x80\xf0\x9f\x98\x80", "[2,{\"d\":2}]", "\xf0\x9f\x98\x80");
    /* inserts of astral text count 2 per character */
    ot_op op;
    CHECK(parse_op("[\"\xf0\x9f\x8f\x97\xef\xb8\x8f\"]", &op) == 0 && op.target_len == 3); /* 🏗️ = pair + VS16 */
    ot_free(&op);
    /* invalid UTF-8 inserts are refused (the JSON parser keeps raw bytes) */
    json *bad = json_arr();
    json_push(bad, json_strn("\xff", 1));
    char err[160];
    CHECK(ot_from_json(bad, 10, &op, err, sizeof err) != 0);
    json_free(bad);
    /* lone surrogates escaped in JSON become U+FFFD (one unit, like the lone unit itself) */
    CHECK(parse_op("[\"\\ud83d\"]", &op) == 0 && op.target_len == 1);
    ot_free(&op);
}

static char *xform(const char *a, const char *b, int which) {
    ot_op oa, ob, a2, b2;
    if (parse_op(a, &oa) != 0 || parse_op(b, &ob) != 0) return xstrdup("ERR");
    int rc = ot_transform(&oa, &ob, &a2, &b2);
    char *s = rc ? xstrdup("ERR") : op_str(which ? &b2 : &a2);
    ot_free(&oa);
    ot_free(&ob);
    if (!rc) { ot_free(&a2); ot_free(&b2); }
    return s;
}

/* apply(apply(s, a), b') and apply(apply(s, b), a') */
static void converge(const char *s, const char *a, const char *b, const char *want) {
    char *b2 = xform(a, b, 1), *a2 = xform(a, b, 0);
    char *sa = app_s(s, a), *sb = app_s(s, b);
    char *x = app_s(sa, b2), *y = app_s(sb, a2);
    CHECK_STR(x, want);
    CHECK_STR(y, want);
    free(b2); free(a2); free(sa); free(sb); free(x); free(y);
}

static void t_transform_vectors(void) {
    /* client: concurrent inserts at one offset, the FIRST argument's insert wins */
    converge("xy", "[1,\"A\",1]", "[1,\"B\",1]", "xABy");
    converge("xy", "[1,\"B\",1]", "[1,\"A\",1]", "xBAy");
    /* client: overlapping deletes remove the span once */
    converge("abcdefgh", "[1,{\"d\":4},3]", "[3,{\"d\":4},1]", "ah");
    /* insert inside a span the other deletes survives */
    converge("abcdef", "[2,\"X\",4]", "[1,{\"d\":4},1]", "aXf");
    converge("abc", "[{\"d\":3}]", "[{\"d\":3}]", "");
    converge("", "[\"a\"]", "[\"b\"]", "ab");
    converge("a\xf0\x9f\x98\x80" "b", "[1,{\"d\":2},1]", "[3,\"\xf0\x9f\x98\x81\",1]", "a\xf0\x9f\x98\x81" "b");
    /* exact transformed shapes */
    char *s = xform("[1,\"A\",1]", "[1,\"B\",1]", 0);
    CHECK_STR(s, "[1,\"A\",2]");
    free(s);
    s = xform("[1,\"A\",1]", "[1,\"B\",1]", 1);
    CHECK_STR(s, "[2,\"B\",1]");
    free(s);
    s = xform("[1,{\"d\":4},3]", "[3,{\"d\":4},1]", 0);
    CHECK_STR(s, "[1,{\"d\":2},1]");
    free(s);
    /* base length mismatch */
    s = xform("[1]", "[2]", 0);
    CHECK_STR(s, "ERR");
    free(s);
}

/* ---------------------------------------------------------------- TP1 on random ops */

static uint64_t rs = 88172645463325252ULL;
static unsigned rnd(unsigned n) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs % n);
}

static const char *const PIECES[] = { "a", "b", "c", " ", "\n", "\xc3\xa9", "\xe2\x80\x94", "\xf0\x9f\x98\x80",
                                      "\xf0\x9f\x8f\x97", "z" };

static char *rand_text(int maxcp) {
    sbuf b;
    sbuf_init(&b);
    int n = (int)rnd((unsigned)maxcp + 1);
    for (int i = 0; i < n; i++) sbuf_puts(&b, PIECES[rnd(10)]);
    if (!b.data) sbuf_reserve(&b, 0);
    return sbuf_take(&b);
}

/* A random op over text, cutting only at code point boundaries. */
static void rand_op(const char *text, ot_op *op) {
    ot_init(op);
    size_t n = strlen(text), i = 0;
    while (i < n) {
        unsigned k = 1 + rnd(4), choice = rnd(5);
        int64_t units = 0;
        size_t j = i;
        for (unsigned c = 0; c < k && j < n; c++) {
            unsigned char b0 = (unsigned char)text[j];
            size_t len = b0 < 0x80 ? 1 : b0 < 0xE0 ? 2 : b0 < 0xF0 ? 3 : 4;
            units += len == 4 ? 2 : 1;
            j += len;
        }
        if (choice == 0) {
            char *s = rand_text(3);
            ot_insert(op, s, strlen(s));
            free(s);
        } else if (choice == 1) {
            ot_delete(op, units);
            i = j;
        } else {
            ot_retain(op, units);
            i = j;
        }
    }
    if (rnd(3) == 0) {
        char *s = rand_text(3);
        ot_insert(op, s, strlen(s));
        free(s);
    }
}

static void t_tp1_random(void) {
    int ok = 1, lens = 1, chained = 1;
    for (int t = 0; t < 3000; t++) {
        char *s = rand_text(24);
        ot_op a, b, a2, b2;
        rand_op(s, &a);
        rand_op(s, &b);
        if (ot_transform(&a, &b, &a2, &b2) != 0) { ok = 0; free(s); ot_free(&a); ot_free(&b); continue; }
        char *sa = app(s, &a), *sb = app(s, &b);
        char *x = app(sa, &b2), *y = app(sb, &a2);
        if (strcmp(x, y) != 0 || strcmp(x, "ERR") == 0) ok = 0;
        if (a2.base_len != b.target_len || b2.base_len != a.target_len) lens = 0;
        /* the server path: an op transformed over two committed ops in a row */
        ot_op c, c2, a3;
        rand_op(sb, &c);
        if (ot_transform(&a2, &c, &a3, &c2) == 0) {
            char *sbc = app(sb, &c);
            char *z = app(sbc, &a3);
            if (strcmp(z, "ERR") == 0) chained = 0;
            free(sbc);
            free(z);
            ot_free(&a3);
            ot_free(&c2);
        } else {
            chained = 0;
        }
        ot_free(&c);
        free(s); free(sa); free(sb); free(x); free(y);
        ot_free(&a); ot_free(&b); ot_free(&a2); ot_free(&b2);
    }
    CHECK(ok);
    CHECK(lens);
    CHECK(chained);
}

/* ---------------------------------------------------------------- db upkeep */

void test_ot_all(void) {
    RUN(t_normalize_apply);
    RUN(t_utf16);
    RUN(t_transform_vectors);
    RUN(t_tp1_random);
}
