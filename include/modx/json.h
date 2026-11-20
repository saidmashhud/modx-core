#ifndef MODX_JSON_H
#define MODX_JSON_H

#include <stddef.h>
#include <stdint.h>
#include "modx/util.h"

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

typedef struct json json;
struct json {
    json_type type;
    int boolean;
    double number;
    char *str;        /* JSON_STRING: NUL-terminated (may contain embedded NULs) */
    size_t str_len;
    size_t len, cap;  /* JSON_ARRAY / JSON_OBJECT */
    json **items;     /* values (array elements or object values) */
    char **keys;      /* JSON_OBJECT keys, insertion ordered */
};

/* Parse exactly one JSON value (surrounded by optional whitespace).
 * Returns NULL on error and writes a message to err (if non-NULL). */
json *json_parse(const char *s, size_t len, char *err, size_t errlen);
void json_free(json *v);

json *json_null(void);
json *json_bool(int b);
json *json_num(double n);
json *json_str(const char *s);
json *json_strn(const char *s, size_t n);
json *json_arr(void);
json *json_obj(void);
json *json_clone(const json *v);
int json_equal(const json *a, const json *b);

/* Objects. json_set takes ownership of val and replaces an existing key. */
json *json_get(const json *obj, const char *key);
void json_set(json *obj, const char *key, json *val);
void json_del(json *obj, const char *key);
int json_has(const json *obj, const char *key);
/* Arrays. Takes ownership. */
void json_push(json *arr, json *val);

/* Typed accessors: return NULL / fallback when the key is missing or the wrong type. */
const char *json_get_str(const json *obj, const char *key);
double json_get_num(const json *obj, const char *key, double fallback);
int json_is_null_or_missing(const json *obj, const char *key);
/* True when v is a finite number with an integral value in the int53 range. */
int json_is_int(const json *v);

void json_write(sbuf *out, const json *v);
void json_write_string(sbuf *out, const char *s, size_t len);
char *json_dump(const json *v);

#endif
