#ifndef MODX_JSON_H
#define MODX_JSON_H

/** @file json.h
 * @brief JSON values: parser, builders, accessors and serializer.
 *
 * Every value is a heap-allocated tree owned by whoever holds the root;
 * json_free() releases it. Containers own their children: json_set() and
 * json_push() take ownership of the value passed in.
 */

#include <stddef.h>
#include <stdint.h>
#include "modx/util.h"

/** Type of a json value. */
typedef enum {
    JSON_NULL,   /**< null */
    JSON_BOOL,   /**< true / false (json::boolean) */
    JSON_NUMBER, /**< number (json::number, a double) */
    JSON_STRING, /**< string (json::str, json::str_len) */
    JSON_ARRAY,  /**< array (json::items) */
    JSON_OBJECT  /**< object (json::keys, json::items) */
} json_type;

/** A JSON value (see struct json). */
typedef struct json json;
/** A JSON value. Only the members of its type are meaningful. */
struct json {
    json_type type;   /**< which members are used */
    int boolean;      /**< JSON_BOOL: 0 or 1 */
    double number;    /**< JSON_NUMBER */
    char *str;        /**< JSON_STRING: NUL-terminated (may contain embedded NULs) */
    size_t str_len;   /**< JSON_STRING: bytes, without the terminator */
    size_t len, cap;  /**< JSON_ARRAY / JSON_OBJECT: items used / allocated */
    json **items;     /**< values (array elements or object values) */
    char **keys;      /**< JSON_OBJECT keys, insertion ordered */
};

/** Parse exactly one JSON value (surrounded by optional whitespace) from
 * @p len bytes of @p s. Nesting is limited to 256 levels; a repeated object
 * key keeps the last value.
 * @return a new value (caller frees), or NULL on error with a message in
 *         @p err (if non-NULL). */
json *json_parse(const char *s, size_t len, char *err, size_t errlen);
/** Frees @p v and everything it contains. NULL is a no-op. */
void json_free(json *v);

/** New null value. */
json *json_null(void);
/** New boolean (any nonzero @p b = true). */
json *json_bool(int b);
/** New number. */
json *json_num(double n);
/** New string, copying the NUL-terminated @p s. */
json *json_str(const char *s);
/** New string, copying @p n bytes of @p s (may contain NULs). */
json *json_strn(const char *s, size_t n);
/** New empty array. */
json *json_arr(void);
/** New empty object. */
json *json_obj(void);
/** Deep copy (NULL for NULL). */
json *json_clone(const json *v);
/** 1 when @p a and @p b are structurally equal (object key order is
 * ignored; numbers compare with ==). Two NULLs are equal. */
int json_equal(const json *a, const json *b);

/** Value of @p key in @p obj (borrowed), or NULL when missing or @p obj is
 * not an object. */
json *json_get(const json *obj, const char *key);
/** Sets @p key in @p obj. Takes ownership of @p val and replaces (frees) an
 * existing value; new keys are appended. */
void json_set(json *obj, const char *key, json *val);
/** Removes @p key from @p obj and frees its value (no-op when missing). */
void json_del(json *obj, const char *key);
/** 1 when @p obj is an object that has @p key. */
int json_has(const json *obj, const char *key);
/** Appends @p val to the array @p arr. Takes ownership. */
void json_push(json *arr, json *val);

/** String value of @p key (borrowed), or NULL when missing or not a string. */
const char *json_get_str(const json *obj, const char *key);
/** Number value of @p key, or @p fallback when missing or not a number. */
double json_get_num(const json *obj, const char *key, double fallback);
/** 1 when @p key is missing or null. */
int json_is_null_or_missing(const json *obj, const char *key);
/** True when @p v is a finite number with an integral value in the int53 range. */
int json_is_int(const json *v);

/** Appends the compact serialization of @p v to @p out (NULL writes null;
 * non-finite numbers are written as null). */
void json_write(sbuf *out, const json *v);
/** Appends @p len bytes of @p s as a quoted, escaped JSON string. */
void json_write_string(sbuf *out, const char *s, size_t len);
/** Serialization of @p v as a new string (caller frees). */
char *json_dump(const json *v);

#endif
