#ifndef MODX_OT_H
#define MODX_OT_H

#include <stddef.h>
#include <stdint.h>
#include "modx/json.h"
#include "modx/util.h"

/** @file ot.h
 * @brief Operational transformation on plain text (the ot.js TextOperation
 * model), with lengths and offsets counted in UTF-16 code units — what
 * JavaScript strings use — while the text itself is stored as UTF-8.
 *
 * Wire form: an array whose items are a number n > 0 (retain n), a non-empty
 * string (insert it) or `{"d": n}` with n > 0 (delete n). An operation spans
 * its base document exactly: retains + deletes = base length; retains +
 * inserts = target length. A retain or delete may never end inside a
 * surrogate pair (an astral code point counts 2 units).
 */

#define OT_MAX_TEXT 1000000   /**< max document length in UTF-16 code units */
#define OT_MAX_ITEMS 10000    /**< max components in one request */

/** Kinds of ot_comp. */
enum { OT_RETAIN = 1, /**< keep n units */ OT_INSERT = 2, /**< insert s */ OT_DELETE = 3 /**< remove n units */ };

/** One component of an operation. */
typedef struct {
    int kind;       /**< OT_RETAIN, OT_INSERT or OT_DELETE */
    int64_t n;      /**< units retained / deleted / inserted (UTF-16) */
    char *s;        /**< insert: owned UTF-8 */
    size_t slen;    /**< insert: bytes */
} ot_comp;

/** A text operation. Zero-initialized or ot_init() = empty; free with ot_free(). */
typedef struct {
    ot_comp *c;                   /**< components (owned) */
    size_t len, cap;              /**< components used / allocated */
    int64_t base_len, target_len; /**< UTF-16 length before / after applying */
} ot_op;

/** Makes @p op empty without freeing anything. */
void ot_init(ot_op *op);
/** Frees the components; @p op is empty afterwards. */
void ot_free(ot_op *op);
/** Appends a retain of @p n units.
 *
 * Builders with ot.js normalization: adjacent components of the same kind
 * merge, and an insert directly after a delete is placed before it. Zero
 * lengths are ignored. */
void ot_retain(ot_op *op, int64_t n);
/** Appends an insert of @p slen bytes of UTF-8 (copied; its UTF-16 length is
 * counted). Normalized like ot_retain(). */
void ot_insert(ot_op *op, const char *s, size_t slen);
/** Appends a delete of @p n units. Normalized like ot_retain(). */
void ot_delete(ot_op *op, int64_t n);

/** UTF-16 length of @p n bytes of UTF-8 text; -1 when it is not valid UTF-8
 * (overlongs, surrogate code points and values above U+10FFFF are invalid). */
int64_t ot_utf16_len(const char *s, size_t n);

/** Parse the wire form (an array of at most @p max_items components) into
 * @p op (initialized by the call; normalized). Rejects other item types,
 * non-integral / non-positive numbers, empty strings, strings with NUL or
 * invalid UTF-8.
 * @return 0, or -1 with a message in @p err (op is then empty). */
int ot_from_json(const json *arr, size_t max_items, ot_op *op, char *err, size_t errlen);
/** The wire form of @p op (a new JSON array; caller frees). */
json *ot_to_json(const ot_op *op);
/** 1 when @p op only retains (changes nothing). */
int ot_is_noop(const ot_op *op);

/** transform(a, b) -> (a2, b2) with apply(apply(S, a), b2) ==
 * apply(apply(S, b), a2) for any S of the shared base length. When both
 * insert at the same position, a's insert goes first. @p a2 and @p b2 are
 * initialized by the call (pass them empty); either may be NULL.
 * @return 0, or -1 when the base lengths differ (a2 / b2 are then empty). */
int ot_transform(const ot_op *a, const ot_op *b, ot_op *a2, ot_op *b2);

/** Apply @p op to UTF-8 @p text (@p len bytes), appending the result to
 * @p out (NULL = validate only). Errors: the span does not match the text's
 * UTF-16 length, a retain / delete boundary splits a surrogate pair, the text
 * is not valid UTF-8. @return 0 or -1 (message in @p err). */
int ot_apply(const char *text, size_t len, const ot_op *op, sbuf *out, char *err, size_t errlen);

#endif
