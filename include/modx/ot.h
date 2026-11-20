#ifndef MODX_OT_H
#define MODX_OT_H

#include <stddef.h>
#include <stdint.h>
#include "modx/json.h"
#include "modx/util.h"

/* Operational transformation on plain text (the ot.js TextOperation
 * model), with lengths and offsets counted in UTF-16 code units — what
 * JavaScript strings use — while the text itself is stored as UTF-8.
 *
 * Wire form: an array whose items are a number n > 0 (retain n), a non-empty
 * string (insert it) or {"d": n} with n > 0 (delete n). An operation spans
 * its base document exactly: retains + deletes = base length; retains +
 * inserts = target length. A retain or delete may never end inside a
 * surrogate pair (an astral code point counts 2 units). */

#define OT_MAX_TEXT 1000000   /* max document length in UTF-16 code units */
#define OT_MAX_ITEMS 10000    /* max components in one request */

enum { OT_RETAIN = 1, OT_INSERT = 2, OT_DELETE = 3 };

typedef struct {
    int kind;
    int64_t n;      /* units retained / deleted / inserted (UTF-16) */
    char *s;        /* insert: owned UTF-8 */
    size_t slen;    /* insert: bytes */
} ot_comp;

typedef struct {
    ot_comp *c;
    size_t len, cap;
    int64_t base_len, target_len;
} ot_op;

void ot_init(ot_op *op);
void ot_free(ot_op *op);
/* Builders with ot.js normalization: adjacent components of the same kind
 * merge, and an insert directly after a delete is placed before it. Zero
 * lengths are ignored. ot_insert copies s. */
void ot_retain(ot_op *op, int64_t n);
void ot_insert(ot_op *op, const char *s, size_t slen);
void ot_delete(ot_op *op, int64_t n);

/* UTF-16 length of UTF-8 text; -1 when it is not valid UTF-8 (overlongs,
 * surrogate code points and values above U+10FFFF are invalid). */
int64_t ot_utf16_len(const char *s, size_t n);

/* Parse the wire form (an array of at most max_items components) into op
 * (normalized). Rejects other item types, non-integral / non-positive
 * numbers, empty strings, strings with NUL or invalid UTF-8. Returns 0, or
 * -1 with a message in err. */
int ot_from_json(const json *arr, size_t max_items, ot_op *op, char *err, size_t errlen);
/* The wire form of op (a new JSON array). */
json *ot_to_json(const ot_op *op);
/* 1 when op only retains (changes nothing). */
int ot_is_noop(const ot_op *op);

/* transform(a, b) -> (a2, b2) with apply(apply(S, a), b2) ==
 * apply(apply(S, b), a2) for any S of the shared base length. When both
 * insert at the same position, a's insert goes first. Returns 0, or -1 when
 * the base lengths differ (a2 / b2 are then empty). Either out may be NULL. */
int ot_transform(const ot_op *a, const ot_op *b, ot_op *a2, ot_op *b2);

/* Apply op to UTF-8 text (len bytes). out NULL = validate only. Errors: the
 * span does not match the text's UTF-16 length, a retain / delete boundary
 * splits a surrogate pair, the text is not valid UTF-8. Returns 0 or -1
 * (message in err). */
int ot_apply(const char *text, size_t len, const ot_op *op, sbuf *out, char *err, size_t errlen);

#endif
