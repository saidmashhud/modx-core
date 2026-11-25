#ifndef MODX_SSE_H
#define MODX_SSE_H

/**
 * @file sse.h
 * @brief Server-Sent Events on top of the HTTP server: a handler turns its response
 * into a stream (mx_sse_open); the hub fans events out to every open stream
 * a filter accepts and keeps idle proxies awake with comment pings.
 *
 * All functions run on the server's loop thread. Pushes go through
 * http_conn_send(), so they respect the durability barrier.
 */

#include <stdint.h>
#include "modx/http.h"

/** Hub over the SSE connections of one server. */
typedef struct {
    http_server *srv;  /**< Server whose connections are streamed to. */
    int64_t ping_ms;   /**< Comment ping interval. */
    int64_t last_ping; /**< Monotonic ms of the last ping round. */
} mx_sse;

/** Initialize a hub for @p srv; ping_ms <= 0 = 15000. */
void mx_sse_init(mx_sse *hub, http_server *srv, int64_t ping_ms);
/** Turn resp into an event stream. app (may be NULL) is bound to the
 * connection (freed by http_server.app_free when it closes). An initial
 * "retry: 2000" line is written. Stream responses are never gzipped or held
 * by the durability barrier. */
void mx_sse_open(http_conn *c, http_response *resp, void *app);
/** Append one event to @p out: "id: <id>\n" (id >= 0), "event: <event>\n" (non-NULL),
 * one "data: " line per line of data (NULL = empty), blank line. */
void mx_sse_format(sbuf *out, const char *event, int64_t id, const char *data);
/** Filter for mx_sse_broadcast(): non-zero = send to @p c. */
typedef int (*mx_sse_filter)(void *ctx, http_conn *c);
/** Send an event to every live stream keep() accepts (keep NULL = all). Returns streams told. */
size_t mx_sse_broadcast(mx_sse *hub, mx_sse_filter keep, void *ctx, const char *event, int64_t id, const char *data);
/** Send an event to one stream. */
void mx_sse_send(http_conn *c, const char *event, int64_t id, const char *data);
/** Comment ping every ping_ms; call from the event loop tick. */
void mx_sse_tick(mx_sse *hub);
/** Number of live SSE streams. */
size_t mx_sse_count(const mx_sse *hub);

#endif
