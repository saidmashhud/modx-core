#include "modx/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 256

static json *jnew(json_type t) {
    json *v = xcalloc(1, sizeof *v);
    v->type = t;
    return v;
}

json *json_null(void) { return jnew(JSON_NULL); }
json *json_bool(int b) { json *v = jnew(JSON_BOOL); v->boolean = b ? 1 : 0; return v; }
json *json_num(double n) { json *v = jnew(JSON_NUMBER); v->number = n; return v; }
json *json_strn(const char *s, size_t n) {
    json *v = jnew(JSON_STRING);
    v->str = xstrndup(s, n);
    v->str_len = n;
    return v;
}
json *json_str(const char *s) { return json_strn(s, strlen(s)); }
json *json_arr(void) { return jnew(JSON_ARRAY); }
json *json_obj(void) { return jnew(JSON_OBJECT); }

void json_free(json *v) {
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->len; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v);
}

static void grow(json *v) {
    if (v->len < v->cap) return;
    v->cap = v->cap ? v->cap * 2 : 4;
    v->items = xrealloc(v->items, v->cap * sizeof *v->items);
    if (v->type == JSON_OBJECT) v->keys = xrealloc(v->keys, v->cap * sizeof *v->keys);
}

void json_push(json *arr, json *val) {
    grow(arr);
    arr->items[arr->len++] = val;
}

static long find_key(const json *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return -1;
    for (size_t i = 0; i < obj->len; i++)
        if (strcmp(obj->keys[i], key) == 0) return (long)i;
    return -1;
}

json *json_get(const json *obj, const char *key) {
    long i = find_key(obj, key);
    return i < 0 ? NULL : obj->items[i];
}

int json_has(const json *obj, const char *key) { return find_key(obj, key) >= 0; }

void json_set(json *obj, const char *key, json *val) {
    long i = find_key(obj, key);
    if (i >= 0) {
        json_free(obj->items[i]);
        obj->items[i] = val;
        return;
    }
    grow(obj);
    obj->keys[obj->len] = xstrdup(key);
    obj->items[obj->len] = val;
    obj->len++;
}

void json_del(json *obj, const char *key) {
    long i = find_key(obj, key);
    if (i < 0) return;
    json_free(obj->items[i]);
    free(obj->keys[i]);
    memmove(&obj->items[i], &obj->items[i + 1], (obj->len - (size_t)i - 1) * sizeof *obj->items);
    memmove(&obj->keys[i], &obj->keys[i + 1], (obj->len - (size_t)i - 1) * sizeof *obj->keys);
    obj->len--;
}

const char *json_get_str(const json *obj, const char *key) {
    json *v = json_get(obj, key);
    return (v && v->type == JSON_STRING) ? v->str : NULL;
}

double json_get_num(const json *obj, const char *key, double fallback) {
    json *v = json_get(obj, key);
    return (v && v->type == JSON_NUMBER) ? v->number : fallback;
}

int json_is_null_or_missing(const json *obj, const char *key) {
    json *v = json_get(obj, key);
    return !v || v->type == JSON_NULL;
}

int json_is_int(const json *v) {
    if (!v || v->type != JSON_NUMBER) return 0;
    double n = v->number;
    return isfinite(n) && n == floor(n) && fabs(n) <= 9007199254740992.0;
}

json *json_clone(const json *v) {
    if (!v) return NULL;
    json *c = jnew(v->type);
    c->boolean = v->boolean;
    c->number = v->number;
    if (v->type == JSON_STRING) { c->str = xstrndup(v->str, v->str_len); c->str_len = v->str_len; }
    if (v->type == JSON_ARRAY || v->type == JSON_OBJECT) {
        c->cap = v->len;
        c->len = v->len;
        c->items = xmalloc((v->len ? v->len : 1) * sizeof *c->items);
        if (v->type == JSON_OBJECT) c->keys = xmalloc((v->len ? v->len : 1) * sizeof *c->keys);
        for (size_t i = 0; i < v->len; i++) {
            c->items[i] = json_clone(v->items[i]);
            if (v->type == JSON_OBJECT) c->keys[i] = xstrdup(v->keys[i]);
        }
    }
    return c;
}

int json_equal(const json *a, const json *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return 0;
    switch (a->type) {
    case JSON_NULL: return 1;
    case JSON_BOOL: return a->boolean == b->boolean;
    case JSON_NUMBER: return a->number == b->number;
    case JSON_STRING: return a->str_len == b->str_len && memcmp(a->str, b->str, a->str_len) == 0;
    case JSON_ARRAY:
        if (a->len != b->len) return 0;
        for (size_t i = 0; i < a->len; i++)
            if (!json_equal(a->items[i], b->items[i])) return 0;
        return 1;
    case JSON_OBJECT:
        if (a->len != b->len) return 0;
        for (size_t i = 0; i < a->len; i++) {
            json *o = json_get(b, a->keys[i]);
            if (!o || !json_equal(a->items[i], o)) return 0;
        }
        return 1;
    }
    return 0;
}

/* ---------------- parser ---------------- */

typedef struct {
    const char *s;
    size_t len, pos;
    int depth;
    char *err;
    size_t errlen;
    int failed;
} parser;

static void perr(parser *p, const char *msg) {
    if (p->failed) return;
    p->failed = 1;
    if (p->err && p->errlen) snprintf(p->err, p->errlen, "%s at offset %zu", msg, p->pos);
}

static void skip_ws(parser *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->pos++;
        else break;
    }
}

static json *parse_value(parser *p);

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex4(parser *p, unsigned *out) {
    if (p->pos + 4 > p->len) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(p->s[p->pos + (size_t)i]);
        if (h < 0) return 0;
        v = (v << 4) | (unsigned)h;
    }
    p->pos += 4;
    *out = v;
    return 1;
}

static void put_utf8(sbuf *b, unsigned cp) {
    char t[4];
    if (cp < 0x80) { t[0] = (char)cp; sbuf_put(b, t, 1); }
    else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); sbuf_put(b, t, 2);
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F)); sbuf_put(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); sbuf_put(b, t, 4);
    }
}

/* Parses a string starting at the opening quote into b. */
static int parse_string_raw(parser *p, sbuf *b) {
    p->pos++; /* opening quote */
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->s[p->pos];
        if (c == '"') { p->pos++; if (!b->data) sbuf_reserve(b, 0); return 1; }
        if (c < 0x20) { perr(p, "control character in string"); return 0; }
        if (c != '\\') {
            /* copy a run of plain bytes (UTF-8 passes through untouched) */
            size_t start = p->pos;
            while (p->pos < p->len) {
                unsigned char d = (unsigned char)p->s[p->pos];
                if (d == '"' || d == '\\' || d < 0x20) break;
                p->pos++;
            }
            sbuf_put(b, p->s + start, p->pos - start);
            continue;
        }
        p->pos++;
        if (p->pos >= p->len) break;
        char e = p->s[p->pos++];
        switch (e) {
        case '"': sbuf_putc(b, '"'); break;
        case '\\': sbuf_putc(b, '\\'); break;
        case '/': sbuf_putc(b, '/'); break;
        case 'b': sbuf_putc(b, '\b'); break;
        case 'f': sbuf_putc(b, '\f'); break;
        case 'n': sbuf_putc(b, '\n'); break;
        case 'r': sbuf_putc(b, '\r'); break;
        case 't': sbuf_putc(b, '\t'); break;
        case 'u': {
            unsigned cp;
            if (!read_hex4(p, &cp)) { perr(p, "bad \\u escape"); return 0; }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned lo;
                if (p->pos + 6 <= p->len && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                    size_t save = p->pos;
                    p->pos += 2;
                    if (read_hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p->pos = save;
                        cp = 0xFFFD;
                    }
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            put_utf8(b, cp);
            break;
        }
        default: perr(p, "bad escape"); return 0;
        }
    }
    perr(p, "unterminated string");
    return 0;
}

static json *parse_string(parser *p) {
    sbuf b;
    sbuf_init(&b);
    if (!parse_string_raw(p, &b)) { sbuf_free(&b); return NULL; }
    json *v = jnew(JSON_STRING);
    v->str_len = b.len;
    v->str = sbuf_take(&b);
    return v;
}

static json *parse_number(parser *p) {
    size_t start = p->pos;
    const char *s = p->s;
    if (p->pos < p->len && s[p->pos] == '-') p->pos++;
    if (p->pos >= p->len) { perr(p, "bad number"); return NULL; }
    if (s[p->pos] == '0') {
        p->pos++;
    } else if (s[p->pos] >= '1' && s[p->pos] <= '9') {
        while (p->pos < p->len && s[p->pos] >= '0' && s[p->pos] <= '9') p->pos++;
    } else { perr(p, "bad number"); return NULL; }
    if (p->pos < p->len && s[p->pos] == '.') {
        p->pos++;
        if (p->pos >= p->len || s[p->pos] < '0' || s[p->pos] > '9') { perr(p, "bad number"); return NULL; }
        while (p->pos < p->len && s[p->pos] >= '0' && s[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->len && (s[p->pos] == 'e' || s[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (s[p->pos] == '+' || s[p->pos] == '-')) p->pos++;
        if (p->pos >= p->len || s[p->pos] < '0' || s[p->pos] > '9') { perr(p, "bad number"); return NULL; }
        while (p->pos < p->len && s[p->pos] >= '0' && s[p->pos] <= '9') p->pos++;
    }
    char tmp[64];
    size_t n = p->pos - start;
    double d;
    if (n < sizeof tmp) {
        memcpy(tmp, s + start, n);
        tmp[n] = 0;
        d = strtod(tmp, NULL);
    } else {
        char *t = xstrndup(s + start, n);
        d = strtod(t, NULL);
        free(t);
    }
    if (!isfinite(d)) { perr(p, "number out of range"); return NULL; }
    return json_num(d);
}

static int match_lit(parser *p, const char *lit) {
    size_t n = strlen(lit);
    if (p->pos + n <= p->len && memcmp(p->s + p->pos, lit, n) == 0) { p->pos += n; return 1; }
    return 0;
}

static json *parse_value(parser *p) {
    skip_ws(p);
    if (p->pos >= p->len) { perr(p, "unexpected end of input"); return NULL; }
    char c = p->s[p->pos];
    if (c == '{') {
        if (++p->depth > JSON_MAX_DEPTH) { perr(p, "nesting too deep"); return NULL; }
        p->pos++;
        json *o = json_obj();
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; p->depth--; return o; }
        for (;;) {
            skip_ws(p);
            if (p->pos >= p->len || p->s[p->pos] != '"') { perr(p, "expected object key"); json_free(o); return NULL; }
            sbuf key;
            sbuf_init(&key);
            if (!parse_string_raw(p, &key)) { sbuf_free(&key); json_free(o); return NULL; }
            skip_ws(p);
            if (p->pos >= p->len || p->s[p->pos] != ':') { perr(p, "expected ':'"); sbuf_free(&key); json_free(o); return NULL; }
            p->pos++;
            json *val = parse_value(p);
            if (!val) { sbuf_free(&key); json_free(o); return NULL; }
            json_set(o, key.data, val);
            sbuf_free(&key);
            skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->len && p->s[p->pos] == '}') { p->pos++; break; }
            perr(p, "expected ',' or '}'");
            json_free(o);
            return NULL;
        }
        p->depth--;
        return o;
    }
    if (c == '[') {
        if (++p->depth > JSON_MAX_DEPTH) { perr(p, "nesting too deep"); return NULL; }
        p->pos++;
        json *a = json_arr();
        skip_ws(p);
        if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; p->depth--; return a; }
        for (;;) {
            json *val = parse_value(p);
            if (!val) { json_free(a); return NULL; }
            json_push(a, val);
            skip_ws(p);
            if (p->pos < p->len && p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->pos < p->len && p->s[p->pos] == ']') { p->pos++; break; }
            perr(p, "expected ',' or ']'");
            json_free(a);
            return NULL;
        }
        p->depth--;
        return a;
    }
    if (c == '"') return parse_string(p);
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(p);
    if (match_lit(p, "true")) return json_bool(1);
    if (match_lit(p, "false")) return json_bool(0);
    if (match_lit(p, "null")) return json_null();
    perr(p, "unexpected character");
    return NULL;
}

json *json_parse(const char *s, size_t len, char *err, size_t errlen) {
    parser p = { s, len, 0, 0, err, errlen, 0 };
    if (err && errlen) err[0] = 0;
    json *v = parse_value(&p);
    if (!v) { if (!p.failed) perr(&p, "parse error"); return NULL; }
    skip_ws(&p);
    if (p.pos != p.len) { perr(&p, "trailing characters"); json_free(v); return NULL; }
    return v;
}

/* ---------------- serializer ---------------- */

void json_write_string(sbuf *out, const char *s, size_t len) {
    static const char hex[] = "0123456789abcdef";
    sbuf_reserve(out, len + 2);
    sbuf_putc(out, '"');
    size_t run = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char ubuf[7];
        switch (c) {
        case '"': esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        case '\b': esc = "\\b"; break;
        case '\f': esc = "\\f"; break;
        default:
            if (c < 0x20) {
                ubuf[0] = '\\'; ubuf[1] = 'u'; ubuf[2] = '0'; ubuf[3] = '0';
                ubuf[4] = hex[c >> 4]; ubuf[5] = hex[c & 15]; ubuf[6] = 0;
                esc = ubuf;
            }
        }
        if (esc) {
            if (run) sbuf_put(out, s + i - run, run);
            run = 0;
            sbuf_puts(out, esc);
        } else {
            run++;
        }
    }
    if (run) sbuf_put(out, s + len - run, run);
    sbuf_putc(out, '"');
}

static void write_number(sbuf *out, double n) {
    if (!isfinite(n)) { sbuf_puts(out, "null"); return; }
    if (n == floor(n) && fabs(n) < 1e15) {
        sbuf_printf(out, "%lld", (long long)n);
        return;
    }
    char tmp[40];
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*g", prec, n);
        if (strtod(tmp, NULL) == n) break;
    }
    sbuf_puts(out, tmp);
}

void json_write(sbuf *out, const json *v) {
    if (!v) { sbuf_puts(out, "null"); return; }
    switch (v->type) {
    case JSON_NULL: sbuf_puts(out, "null"); break;
    case JSON_BOOL: sbuf_puts(out, v->boolean ? "true" : "false"); break;
    case JSON_NUMBER: write_number(out, v->number); break;
    case JSON_STRING: json_write_string(out, v->str, v->str_len); break;
    case JSON_ARRAY:
        sbuf_putc(out, '[');
        for (size_t i = 0; i < v->len; i++) {
            if (i) sbuf_putc(out, ',');
            json_write(out, v->items[i]);
        }
        sbuf_putc(out, ']');
        break;
    case JSON_OBJECT:
        sbuf_putc(out, '{');
        for (size_t i = 0; i < v->len; i++) {
            if (i) sbuf_putc(out, ',');
            json_write_string(out, v->keys[i], strlen(v->keys[i]));
            sbuf_putc(out, ':');
            json_write(out, v->items[i]);
        }
        sbuf_putc(out, '}');
        break;
    }
}

char *json_dump(const json *v) {
    sbuf b;
    sbuf_init(&b);
    json_write(&b, v);
    return sbuf_take(&b);
}
