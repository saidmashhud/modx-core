#ifndef MODX_SSE_H
#define MODX_SSE_H

/* Server-Sent Events on top of the HTTP server: a handler turns its response
 * into a stream (mx_sse_open); the hub fans events out to every open stream
 * a filter accepts and keeps idle proxies awake with comment pings. */

#include <stdint.h>
#include "modx/http.h"

typedef struct {
    http_server *srv;
    int64_t ping_ms;
    int64_t last_ping;
} mx_sse;

void mx_sse_init(mx_sse *hub, http_server *srv, int64_t ping_ms);
/* Turn resp into an event stream. app (may be NULL) is bound to the
 * connection (freed by http_server.app_free when it closes). An initial
 * "retry:" line is written. */
void mx_sse_open(http_conn *c, http_response *resp, void *app);
/* Append one event: "id: <id>\n" (id >= 0), "event: <event>\n" (non-NULL),
 * one "data: " line per line of data, blank line. */
void mx_sse_format(sbuf *out, const char *event, int64_t id, const char *data);
/* Send an event to every stream keep() accepts (keep NULL = all). Returns streams told. */
typedef int (*mx_sse_filter)(void *ctx, http_conn *c);
size_t mx_sse_broadcast(mx_sse *hub, mx_sse_filter keep, void *ctx, const char *event, int64_t id, const char *data);
/* Send to one stream. */
void mx_sse_send(http_conn *c, const char *event, int64_t id, const char *data);
/* Comment ping every ping_ms; call from the event loop tick. */
void mx_sse_tick(mx_sse *hub);
size_t mx_sse_count(const mx_sse *hub);

#endif
