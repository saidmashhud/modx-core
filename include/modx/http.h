#ifndef MODX_HTTP_H
#define MODX_HTTP_H

/**
 * @file http.h
 * @brief HTTP/1.1 server: request parser, single-threaded event loop
 * (kqueue / epoll / poll), keep-alive and pipelining, file streaming, gzip,
 * SSE and WebSocket upgrades, deferred responses, watched descriptors and a
 * durability barrier (group commit) for responses that report writes.
 *
 * Everything runs on the loop thread: handlers, ticks, watch callbacks and
 * http_deferred_finish(). None of these functions is thread-safe.
 */

#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include "modx/util.h"

#define HTTP_MAX_BODY (1024 * 1024)          /**< Default per-request body limit; larger bodies need http_server.body_limit. */
#define HTTP_MAX_UPLOAD (50 * 1024 * 1024)   /**< Hard ceiling for routes that opt in (uploads); larger requests get 413. */
#define HTTP_READ_BUDGET (1024 * 1024)       /**< Bytes read from one connection per poll round. */
#define HTTP_FILE_CHUNK (256 * 1024)         /**< File responses are streamed in chunks of this size. */
#define HTTP_MAX_HEADER (64 * 1024)          /**< Request line + headers limit; larger requests get 431. */
#define HTTP_MAX_CONNS 1024                  /**< Default connection limit (http_server.max_conns). */
#define HTTP_IDLE_MS 60000             /**< Keep-alive connection without a request is closed after this. */
#define HTTP_HEADER_TIMEOUT_MS 10000   /**< First byte of a request -> end of its headers. */
#define HTTP_BODY_TIMEOUT_MS 60000     /**< First byte of a request -> end of its body. */
#define HTTP_WRITE_TIMEOUT_MS 60000    /**< Pending output without progress. */
#define HTTP_DRAIN_MS 5000             /**< Graceful shutdown budget. */
#define HTTP_LINGER_MS 2000            /**< After a final response: read and discard this long before closing. */
#define HTTP_SSE_MAX_BACKLOG (8 * 1024 * 1024) /**< Unsent bytes per connection above which http_conn_send() marks it dead. */
#define HTTP_DURABLE_UNAVAILABLE (-2)  /**< durable_commit() result: storage refuses writes -> held responses become 503. */

/** One key / value pair (query parameter or header). Both strings are owned by the request. */
typedef struct {
    char *key; /**< Name (header names are lower-cased). */
    char *val; /**< Value. */
} http_kv;

/** A parsed request. Filled by http_parse_request*(), released with http_request_free(). */
typedef struct {
    char method[16];   /**< Upper-case method, e.g. "GET". */
    char *target;      /**< Raw request target. */
    char *path;        /**< Percent-decoded path (no query). */
    http_kv *query;    /**< Decoded query params ('+' as space). */
    size_t nquery;     /**< Entries in @c query. */
    http_kv *headers;  /**< Header names lower-cased, values trimmed. */
    size_t nheaders;   /**< Entries in @c headers. */
    char *body;        /**< NUL-terminated copy of the body; for bodies above HTTP_MAX_BODY (raw
                          uploads) a borrowed, NOT NUL-terminated view into the connection's
                          input buffer, valid until the handler returns (no 50 MiB copy). */
    size_t body_len;   /**< Body length in bytes (Content-Length). */
    int body_borrowed; /**< 1 when @c body is a borrowed view (not freed by http_request_free()). */
    int keep_alive;    /**< 1 when the connection stays open (HTTP/1.1 default, Connection header). */
} http_request;

/**
 * The response a handler fills. The server initializes it to status 200,
 * content type "application/json; charset=utf-8", empty body and headers,
 * file_fd -1, and frees body / headers after the handler returns.
 */
typedef struct {
    int status;               /**< HTTP status code. */
    const char *content_type; /**< Content-Type value (not copied; must outlive the response), or NULL for none. */
    sbuf body;                /**< Response body. Not sent for HEAD, 204 and 304. */
    sbuf headers;      /**< Extra "Name: value\r\n" lines. */
    int sse;           /**< Switch the connection into an SSE stream (set by mx_sse_open). */
    int ws;            /**< 101: switch the connection to WebSocket (set by mx_ws_accept). */
    int file_fd;       /**< >= 0: the body is file_len bytes streamed from this fd (taken over
                          and closed by the server); body must be empty. -1 = none. */
    size_t file_len;   /**< Bytes to stream from @c file_fd. */
    int deferred;      /**< Set by http_defer: this response is ignored; the real one comes later. */
} http_response;

/** One client connection (owned by the server). */
typedef struct http_conn http_conn;
/** The server and its event loop. Zero it, set listen_fd = -1, fill handler and options. */
typedef struct http_server http_server;
/** Handle for a response finished later (http_defer). Opaque. */
typedef struct http_deferred http_deferred;

/** Callback for a descriptor watched by the server's poller (http_server_watch). */
typedef void (*http_fd_cb)(http_server *srv, int fd, void *ctx);
/** A watched descriptor (internal bookkeeping of http_server_watch). */
typedef struct {
    int fd;        /**< Watched descriptor. */
    http_fd_cb cb; /**< Called on the loop thread when @c fd is readable. */
    void *ctx;     /**< Passed to @c cb. */
} http_watch;

/**
 * Request handler, called on the loop thread with a parsed request and a
 * prepared response. @p ctx is http_server.ctx. @p req and @p resp are only
 * valid during the call (use http_defer to answer later).
 */
typedef void (*http_handler)(http_server *srv, http_conn *c, const http_request *req,
                             http_response *resp, void *ctx);
/** Called once per loop iteration (after waiting for events, before the barrier). */
typedef void (*http_tick)(http_server *srv, void *ctx);
/** Body limit for a request whose method, path and headers are parsed (body not
 * yet). Only consulted for bodies above HTTP_MAX_BODY. Returning more than
 * HTTP_MAX_UPLOAD is clamped to HTTP_MAX_UPLOAD. */
typedef size_t (*http_body_limit_fn)(const http_request *partial, void *ctx);

/** A client connection. Fields are maintained by the server; applications
 * normally read @c ip, @c app and @c srv only. */
struct http_conn {
    int fd;            /**< Socket. */
    sbuf in;           /**< Received, not yet processed input. */
    sbuf out;          /**< Output queued for the socket. */
    size_t out_off;    /**< Bytes of @c out already written. */
    int close_after_write; /**< Close once @c out is flushed. */
    int is_sse;        /**< The connection is an SSE stream. */
    int sent_continue; /**< "100 Continue" already sent for the request being received. */
    int64_t last_active; /**< Monotonic ms of the last activity. */
    int dead;          /**< Marked for closing; freed at the end of the loop iteration. */
    size_t need;       /**< Bytes the request being received needs in `in` (0 = unknown). */
    int send_fd;       /**< File being streamed after `out` drains (-1 = none). */
    size_t send_left;  /**< Bytes of @c send_fd still to send. */
    int resume;        /**< Input left unprocessed while a file was streaming or a response was deferred. */
    char ip[64];       /**< Peer address: "203.0.113.7" or "2001:db8::7" (IPv4-mapped shown as IPv4). */
    int64_t req_start; /**< Mono ms of the first byte of the request being received (0 = none). */
    int64_t out_since; /**< Mono ms output has been waiting / last write progress (0 = none). */
    int64_t requests;  /**< Requests served on this connection. */
    int64_t linger_until; /**< Lingering close: output done, input discarded until then (0 = no). */
    int is_ws;         /**< WebSocket after a 101. */
    int ws_closing;    /**< Close frame sent. */
    int ws_msg_op;     /**< Opcode of the message being reassembled (0 = none). */
    sbuf ws_msg;       /**< Fragments so far. */
    int64_t ws_ping_due; /**< A ping is out; the peer must answer before this (0 = none). */
    int want_write;    /**< Registered for writability with kqueue / epoll. */
    http_server *srv;  /**< The server that accepted it. */
    sbuf deferred;     /**< Group commit: pushes queued while changes were pending, sent after them. */
    http_deferred *pending; /**< A response the application finishes later (http_defer); further input waits. */
    int held;          /**< Group commit: output waits for the durability barrier. */
    size_t hold_mark;  /**< out.len before the first held response. */
    void *app;         /**< Application data bound to the connection (e.g. an SSE subscription);
                          freed with http_server.app_free. */
};

/**
 * Server state and options. Zero-initialize, set listen_fd = -1, set
 * @c handler (required) and any options, then http_server_listen() and
 * http_server_run(). Option fields left 0 / NULL take the documented default.
 */
struct http_server {
    int listen_fd;               /**< Listening socket (-1 = none). */
    int port;                    /**< Bound port (after http_server_listen). */
    int listen_family;           /**< AF_INET or AF_INET6 (after http_server_listen). */
    int listen_dual_stack;       /**< 1: an IPv6 socket that takes IPv4 clients too. */
    http_conn **conns;           /**< Open connections. */
    size_t nconns;               /**< Entries in @c conns. */
    http_handler handler;        /**< Request handler (required). */
    http_tick tick;              /**< Per-iteration callback (NULL = none). */
    http_body_limit_fn body_limit; /**< NULL = HTTP_MAX_BODY for everything. */
    /** Called for every connection with app set right before the loop frees it
     * (never on shutdown); every connection is still valid during the call. */
    void (*on_close)(http_server *srv, http_conn *c, void *ctx);
    void (*app_free)(void *app);   /**< Frees http_conn.app (NULL = free()). */
    size_t max_conns;              /**< 0 = HTTP_MAX_CONNS; connections beyond it are closed on accept. */
    int64_t idle_ms;               /**< 0 = HTTP_IDLE_MS. */
    int64_t header_timeout_ms;     /**< 0 = HTTP_HEADER_TIMEOUT_MS (slowloris). */
    int64_t body_timeout_ms;       /**< 0 = HTTP_BODY_TIMEOUT_MS. */
    int64_t write_timeout_ms;      /**< 0 = HTTP_WRITE_TIMEOUT_MS. */
    int64_t drain_ms;              /**< 0 = HTTP_DRAIN_MS. */
    int64_t timeouts;              /**< Counter: connections dropped by a timeout. */
    int64_t last_timeout_check;    /**< Internal: mono ms of the last timeout sweep. */
    int draining;                  /**< Set during graceful shutdown. */
    size_t nconns_peak;            /**< Counter: highest @c nconns seen. */
    /** @name Durability barrier (group commit)
     * When set, a response produced while durable_pending() is true is held back; once per loop
     * iteration durable_commit() runs (one write + one fsync for everything the
     * iteration's requests changed) and only then are held responses sent.
     * If the commit fails (non-zero) they are replaced by 500 responses, or
     * by 503 responses when it returns HTTP_DURABLE_UNAVAILABLE or less (the
     * storage refuses writes, e.g. a store that failed earlier); either way
     * the connection closes after it.
     * @{ */
    int (*durable_pending)(void *dctx); /**< Non-zero while uncommitted changes exist. */
    int (*durable_commit)(void *dctx);  /**< Makes the changes durable: 0, non-zero = failure, <= HTTP_DURABLE_UNAVAILABLE = 503. */
    void *durable_ctx;                  /**< Passed to both callbacks. */
    /** @} */
    size_t gzip_min;             /**< Gzip compressible 200 responses of at least this many bytes (0 = off). */
    int gzip_level;              /**< 1..9, 0 = 6. */
    int64_t gzip_in, gzip_out;   /**< Counters: bytes before / after compression. */
    /** WebSocket: a complete message (text / binary) arrived. data is valid during the call; ctx is http_server.ctx. */
    void (*on_ws_message)(http_server *srv, http_conn *c, int opcode, const char *data, size_t len, void *ctx);
    size_t ws_max_message;       /**< 0 = MX_WS_MAX_MESSAGE. */
    int64_t ws_ping_ms;          /**< 0 = MX_WS_PING_MS. */
    int64_t ws_messages;         /**< Counter: WebSocket messages received. */
    int use_poll;                /**< 1 = poll() even where kqueue / epoll exist. */
    int poll_fd;                 /**< Kqueue / epoll descriptor (-1 = poll()). */
    struct pollfd *pfds;         /**< poll() fallback set. */
    size_t pcap;                 /**< Capacity of @c pfds. */
    int running;                 /**< 1 while http_server_run() runs. */
    int in_barrier;              /**< 1 while durable_commit() runs (pushes go straight out). */
    int64_t commits;             /**< Counter: barrier commits run. */
    int64_t commit_failures;     /**< Counter: commits that failed. */
    void *ctx;                   /**< Passed to handler, tick, on_close, on_ws_message and body_limit. */
    http_watch *watches;         /**< Extra descriptors (http_server_watch), e.g. a worker pool's wake-up. */
    size_t nwatches;             /**< Entries in @c watches. */
    int64_t deferred_started, deferred_finished, deferred_orphaned; /**< Counters: http_defer calls, finishes, and deferred responses whose connection closed first. */
    int wake_now;                /**< The next wait does not block (work queued outside the event path). */
};

/** First value of header @p lname (lower-case name), or NULL. */
const char *http_req_header(const http_request *r, const char *lname);
/** Last occurrence of a header (e.g. the X-Forwarded-For a proxy appended), or NULL. */
const char *http_req_header_last(const http_request *r, const char *lname);
/** First value of query parameter @p key, or NULL. */
const char *http_req_query(const http_request *r, const char *key);
/** Reason phrase for a status code ("Unknown" for codes it does not list). */
const char *http_status_text(int status);

/** Percent-decode s[0..n); plus_as_space for query strings. Returns a malloc'd
 * string (caller frees), or NULL if a NUL byte would result. Invalid escapes are kept as is. */
char *http_url_decode(const char *s, size_t n, int plus_as_space);

/** Parse one request from buf. Returns >0 = bytes consumed (request filled),
 * 0 = need more data, <0 = -HTTP status to reply with before closing
 * (400, 411 for Transfer-Encoding, 413, 431).
 * Bodies above HTTP_MAX_BODY are borrowed from buf (body_borrowed).
 * *expect_continue is set when the client sent "Expect: 100-continue" and the
 * body is not complete yet. On success free @p req with http_request_free(). */
long http_parse_request(const char *buf, size_t len, http_request *req, int *expect_continue);
/** Same, with a per-request body limit callback (may be NULL). */
long http_parse_request_ex(const char *buf, size_t len, http_request *req, int *expect_continue,
                           http_body_limit_fn limit, void *ctx);
/** Same; when 0 is returned with the headers complete, *need (if non-NULL) is
 * the total byte count the request needs (headers + body). */
long http_parse_request_need(const char *buf, size_t len, http_request *req, int *expect_continue,
                             http_body_limit_fn limit, void *ctx, size_t *need);
/** Frees what the parser allocated (not a borrowed body) and zeroes @p r. */
void http_request_free(http_request *r);

/**
 * Listens on host:port (port 0 = any free port; srv->port is the one bound).
 * host is a numeric address or a name:
 * @verbatim
 * "127.0.0.1", "0.0.0.0"        IPv4 only
 * "::1", "[::1]", "2001:db8::5" IPv6 only (IPV6_V6ONLY)
 * "::", "*", "" or NULL         every address: dual stack (IPv4 clients
 *                               arrive as ::ffff:a.b.c.d and are shown as
 *                               a.b.c.d in http_conn.ip); falls back to
 *                               0.0.0.0 on a host without IPv6
 * "localhost" etc.              the first address the name resolves to
 * @endverbatim
 * Sets listen_fd, port, listen_family and listen_dual_stack.
 * Returns 0, or -1 with errno (EINVAL for a bad port or unresolvable host).
 */
int http_server_listen(http_server *srv, const char *host, int port);
/* Forward declaration for http_addr_format. */
struct sockaddr;
/** Formats an AF_INET / AF_INET6 socket address without the port into
 * out[0..cap); an IPv4-mapped IPv6 address (::ffff:a.b.c.d) as plain IPv4.
 * Returns 1 = ok, 0 = other family, NULL or too small (out is then ""). */
int http_addr_format(const struct sockaddr *sa, char *out, size_t cap);
/** "kqueue", "epoll" or "poll" (valid once the server runs). */
const char *http_server_poller(const http_server *srv);
/** Runs until *stop becomes non-zero. Uses kqueue (macOS / BSD) or epoll
 * (Linux), poll() elsewhere or with use_poll / -DMODX_POLLER_POLL.
 *
 * On return (after *stop) the server has drained: no new connections,
 * event streams closed, requests already arriving answered, held responses
 * committed and flushed (bounded by drain_ms). */
void http_server_run(http_server *srv, volatile sig_atomic_t *stop);
/** Runs a final barrier, flushes what it can without blocking, frees every
 * connection and closes the listening and poller descriptors. Watched fds stay open. */
void http_server_close(http_server *srv);

/** Queue bytes on a connection (SSE / WebSocket pushes). Marks it dead on
 * overflow (more than HTTP_SSE_MAX_BACKLOG unsent); no-op on a dead connection.
 * With a durability barrier, bytes pushed while changes are
 * pending wait for the commit and go out right after the change
 * notifications it triggers (dropped if the commit fails), so nothing is
 * announced before what it refers to is durable. */
void http_conn_send(http_conn *c, const char *data, size_t len);

/** Calls cb on the loop thread whenever fd is readable (level-triggered; cb
 * must consume what made it readable). Works before or while the server runs,
 * with every poller. One callback per fd. Returns 0, or -1 with errno
 * (EINVAL: fd < 0 or cb NULL; EEXIST: already watched; or the poller's error). */
int http_server_watch(http_server *srv, int fd, http_fd_cb cb, void *ctx);
/** Stops watching fd (the descriptor stays open). Returns 0, or -1 (ENOENT) if unknown. */
int http_server_unwatch(http_server *srv, int fd);

/**
 * Defers the response to @p req. Call inside the handler.
 *
 * Deferred responses: the handler starts work that finishes later on the loop
 * thread (a worker pool job such as password_verify_async), so the loop keeps
 * serving other connections meanwhile. Called inside the handler, http_defer
 * makes the server ignore resp and wait for http_deferred_finish(); further
 * (pipelined) requests on that connection wait too, so answers stay in order.
 * Timeouts for idle connections do not apply while it waits; a graceful drain
 * waits for it (bounded by drain_ms).
 *
 * Finish on the loop thread: fill http_deferred_response() like a handler
 * fills resp (status, body, headers, content type; not SSE / WebSocket),
 * then call http_deferred_finish(), which sends it, or with a durability
 * barrier holds it until the changes made so far are durable, exactly like
 * a handler's response, then frees the handle. If the client went away in
 * between, the response object is still valid (writes are harmless) and
 * finish just frees it (counted in deferred_orphaned); http_deferred_alive() tells.
 * Exactly one finish per http_defer.
 *
 * @return The handle (owned by the server until http_deferred_finish()). Its
 * response starts as status 200, "application/json; charset=utf-8", empty.
 */
http_deferred *http_defer(http_conn *c, const http_request *req, http_response *resp);
/** The response to fill before http_deferred_finish(); valid until then. */
http_response *http_deferred_response(http_deferred *d);
/** The connection, or NULL once it has been closed. */
http_conn *http_deferred_conn(const http_deferred *d);
/** 1 while the connection is open and not marked dead. */
int http_deferred_alive(const http_deferred *d);
/** Sends (or holds for the barrier) the response, lets pipelined requests run,
 * and frees @p d. Loop thread only; exactly once per http_defer(). */
void http_deferred_finish(http_deferred *d);

#endif
