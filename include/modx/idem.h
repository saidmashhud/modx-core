#ifndef MODX_IDEM_H
#define MODX_IDEM_H

/** @file idem.h
 * @brief Idempotency keys: replay the first completed answer to a retried
 * write instead of executing it again.
 *
 * A client that retries a write (its answer was lost) sends the same
 * `Idempotency-Key: <key>` with every attempt; the server remembers the
 * first completed answer for that key and replays it instead of executing
 * the write again.
 *
 * This module is the mechanism; the application decides the policy:
 *
 *   - which collection holds the records (a private, no-ring collection of
 *     its store: persisted, never broadcast; MX_COLL_PRIVATE | MX_COLL_NO_RING)
 *   - the record id, i.e. the scope of a key (e.g. `"<account>|<tenant>|<key>"`)
 *   - retention (mx_idem_prune(): age; mx_idem_cap(): count per owner)
 *
 * A record is a JSON row:
 * @verbatim
   {id, method, target, status, content_type, body | body_omitted:true,
    created_at, ...fields the application adds (owner, scope)}
   @endverbatim
 *
 * Typical use in a handler:
 * @verbatim
   key = http_req_header(req, MX_IDEM_HEADER)
   if (key && mx_idem_method(req->method)):
     if (!mx_idem_key_valid(key)) -> 400
     rec = lookup(id(key)); if expired: treat as absent
     if (rec && !mx_idem_matches(rec, req)) -> 422 idempotency_mismatch
     if (rec) { mx_idem_replay(rec, resp); return; }
   ... execute the write ...
   if (mx_idem_storable(resp)) put(mx_idem_record(id, req, resp, now, max_body))
   @endverbatim
 *
 * Put the record in the same store batch as the write (before the durability
 * barrier commits): one fsync makes both durable, or neither.
 */

#include <stddef.h>
#include <stdint.h>
#include "modx/http.h"
#include "modx/json.h"
#include "modx/store.h"

#define MX_IDEM_HEADER "idempotency-key"   /**< request header name, lower-cased as http_req_header() wants it */
#define MX_IDEM_KEY_MAX 128                /**< longest accepted key (bytes) */
#define MX_IDEM_REPLAYED_HEADER "Idempotent-Replayed: true\r\n" /**< response header line a replay adds */

/** 1 for a key of 1..MX_IDEM_KEY_MAX printable ASCII characters (0x20..0x7e;
 * a key may not start or end with a space). */
int mx_idem_key_valid(const char *key);
/** 1 for the methods a key applies to: POST, PUT, PATCH, DELETE. */
int mx_idem_method(const char *method);
/** 1 when the response may be remembered: 2xx and 4xx (except 408 and 429,
 * which ask the client to retry) that are buffered (no SSE / WebSocket /
 * file body). 5xx and I/O failures are never remembered, so they can be
 * retried. */
int mx_idem_storable(const http_response *resp);
/** A new record row for the answer (caller owns it). A body larger than
 * @p max_body, or one that is not valid UTF-8 text (or contains NUL), is not
 * stored: the record gets body_omitted:true and a replay answers the same
 * status with `{"replayed":true,"body_omitted":true}`.
 * @param now created_at to record (Unix ms) */
json *mx_idem_record(const char *id, const http_request *req, const http_response *resp, int64_t now,
                     size_t max_body);
/** 1 when @p rec was made for the same method and target (path + query). */
int mx_idem_matches(const json *rec, const http_request *req);
/** 1 when @p rec is at least @p ttl_ms old at @p now (a record without
 * created_at is expired). */
int mx_idem_expired(const json *rec, int64_t now, int64_t ttl_ms);
/** Fill @p resp from @p rec: status, content type, body, and the
 * "Idempotent-Replayed: true" header. resp->content_type may borrow from rec,
 * which must stay in the store until the response is serialized (true for the
 * handler call that replays it). */
void mx_idem_replay(const json *rec, http_response *resp);

/** Delete the records of @p coll created before @p cutoff, and those without
 * a numeric created_at (journaled deletes; call mx_store_sync() after).
 * @return the number deleted, or -1 when the store refused a delete (it has
 *         failed: mx_store_failed()). */
long mx_idem_prune(mx_store *st, int coll, int64_t cutoff);
/** Make room for one more record of the owner whose @p field equals
 * @p value: delete its oldest records (by created_at, then id) until fewer
 * than @p max remain. @return the number deleted, or -1 when the store
 * refused a delete (it has failed). */
long mx_idem_cap(mx_store *st, int coll, const char *field, const char *value, size_t max);

#endif
