#ifndef MODX_HTTP_H
#define MODX_HTTP_H

#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include "modx/util.h"

#define HTTP_MAX_BODY (1024 * 1024)          /* default per-request body limit */
#define HTTP_MAX_UPLOAD (50 * 1024 * 1024)   /* hard ceiling for routes that opt in (uploads) */
#define HTTP_READ_BUDGET (1024 * 1024)       /* bytes read from one connection per poll round */
#define HTTP_FILE_CHUNK (256 * 1024)         /* file responses are streamed in chunks of this size */
#define HTTP_MAX_HEADER (64 * 1024)
#define HTTP_MAX_CONNS 1024
#define HTTP_IDLE_MS 60000             /* keep-alive connection without a request */
#define HTTP_HEADER_TIMEOUT_MS 10000   /* first byte of a request -> end of its headers */
#define HTTP_BODY_TIMEOUT_MS 60000     /* first byte of a request -> end of its body */
#define HTTP_WRITE_TIMEOUT_MS 60000    /* pending output without progress */
#define HTTP_DRAIN_MS 5000             /* graceful shutdown budget */
#define HTTP_LINGER_MS 2000            /* after a final response: read and discard this long before closing */
#define HTTP_SSE_MAX_BACKLOG (8 * 1024 * 1024)
#define HTTP_DURABLE_UNAVAILABLE (-2)  /* durable_commit: storage refuses writes -> held responses become 503 */

typedef struct {
    char *key;
    char *val;
} http_kv;

typedef struct {
    char method[16];
    char *target;      /* raw request target */
    char *path;        /* percent-decoded path (no query) */
    http_kv *query;    /* decoded query params */
    size_t nquery;
    http_kv *headers;  /* header names lower-cased */
    size_t nheaders;
    char *body;        /* NUL-terminated copy of the body; for bodies above HTTP_MAX_BODY (raw
                          uploads) a borrowed, NOT NUL-terminated view into the connection's
                          input buffer, valid until the handler returns (no 50 MiB copy) */
    size_t body_len;
    int body_borrowed;
    int keep_alive;
} http_request;

typedef struct {
    int status;
    const char *content_type;
    sbuf body;
    sbuf headers;      /* extra "Name: value\r\n" lines */
    int sse;           /* switch the connection into an SSE stream */
    int ws;            /* 101: switch the connection to WebSocket (set by mx_ws_accept) */
    int file_fd;       /* >= 0: the body is file_len bytes streamed from this fd (taken over
                          and closed by the server); body must be empty. -1 = none */
    size_t file_len;
} http_response;

typedef struct http_conn http_conn;
typedef struct http_server http_server;

typedef void (*http_handler)(http_server *srv, http_conn *c, const http_request *req,
                             http_response *resp, void *ctx);
typedef void (*http_tick)(http_server *srv, void *ctx);
/* Body limit for a request whose method, path and headers are parsed (body not
 * yet). Returning more than HTTP_MAX_BODY is clamped to HTTP_MAX_UPLOAD. */
typedef size_t (*http_body_limit_fn)(const http_request *partial, void *ctx);

struct http_conn {
    int fd;
    sbuf in;
    sbuf out;
    size_t out_off;
    int close_after_write;
    int is_sse;
    int sent_continue;
    int64_t last_active;
    int dead;
    size_t need;       /* bytes the request being received needs in `in` (0 = unknown) */
    int send_fd;       /* file being streamed after `out` drains (-1 = none) */
    size_t send_left;
    int resume;        /* input left unprocessed while a file was streaming */
    char ip[64];       /* peer address */
    int64_t req_start; /* mono ms of the first byte of the request being received (0 = none) */
    int64_t out_since; /* mono ms output has been waiting / last write progress (0 = none) */
    int64_t requests;  /* requests served on this connection */
    int64_t linger_until; /* lingering close: output done, input discarded until then (0 = no) */
    int is_ws;         /* WebSocket after a 101 */
    int ws_closing;    /* close frame sent */
    int ws_msg_op;     /* opcode of the message being reassembled (0 = none) */
    sbuf ws_msg;       /* fragments so far */
    int64_t ws_ping_due; /* a ping is out; the peer must answer before this (0 = none) */
    int want_write;    /* registered for writability with kqueue / epoll */
    http_server *srv;  /* the server that accepted it */
    sbuf deferred;     /* group commit: pushes queued while changes were pending, sent after them */
    int held;          /* group commit: output waits for the durability barrier */
    size_t hold_mark;  /* out.len before the first held response */
    void *app;         /* application data bound to the connection (e.g. an SSE subscription);
                          freed with http_server.app_free */
};

struct http_server {
    int listen_fd;
    int port;
    http_conn **conns;
    size_t nconns;
    http_handler handler;
    http_tick tick;
    http_body_limit_fn body_limit; /* NULL = HTTP_MAX_BODY for everything */
    /* Called for every connection with app set right before the loop frees it
     * (never on shutdown); every connection is still valid during the call. */
    void (*on_close)(http_server *srv, http_conn *c, void *ctx);
    void (*app_free)(void *app);   /* frees http_conn.app (NULL = free()) */
    size_t max_conns;              /* 0 = HTTP_MAX_CONNS */
    int64_t idle_ms;               /* 0 = HTTP_IDLE_MS */
    int64_t header_timeout_ms;     /* 0 = HTTP_HEADER_TIMEOUT_MS (slowloris) */
    int64_t body_timeout_ms;       /* 0 = HTTP_BODY_TIMEOUT_MS */
    int64_t write_timeout_ms;      /* 0 = HTTP_WRITE_TIMEOUT_MS */
    int64_t drain_ms;              /* 0 = HTTP_DRAIN_MS */
    int64_t timeouts;              /* connections dropped by a timeout */
    int64_t last_timeout_check;
    int draining;
    size_t nconns_peak;
    /* Durability barrier (group commit). When set, a response produced while
     * durable_pending() is true is held back; once per loop iteration
     * durable_commit() runs (one write + one fsync for everything the
     * iteration's requests changed) and only then are held responses sent.
     * If the commit fails (non-zero) they are replaced by 500 responses, or
     * by 503 responses when it returns HTTP_DURABLE_UNAVAILABLE or less (the
     * storage refuses writes, e.g. a store that failed earlier). */
    int (*durable_pending)(void *dctx);
    int (*durable_commit)(void *dctx);
    void *durable_ctx;
    size_t gzip_min;             /* gzip compressible responses of at least this many bytes (0 = off) */
    int gzip_level;              /* 1..9, 0 = 6 */
    int64_t gzip_in, gzip_out;   /* bytes before / after compression */
    /* WebSocket: a complete message (text / binary) arrived. data is valid during the call. */
    void (*on_ws_message)(http_server *srv, http_conn *c, int opcode, const char *data, size_t len, void *ctx);
    size_t ws_max_message;       /* 0 = MX_WS_MAX_MESSAGE */
    int64_t ws_ping_ms;          /* 0 = MX_WS_PING_MS */
    int64_t ws_messages;
    int use_poll;                /* 1 = poll() even where kqueue / epoll exist */
    int poll_fd;                 /* kqueue / epoll descriptor (-1 = poll()) */
    struct pollfd *pfds;         /* poll() fallback set */
    size_t pcap;
    int running;
    int in_barrier;
    int64_t commits;             /* barrier commits run */
    int64_t commit_failures;
    void *ctx;
};

const char *http_req_header(const http_request *r, const char *lname);
/* Last occurrence of a header (e.g. the X-Forwarded-For a proxy appended). */
const char *http_req_header_last(const http_request *r, const char *lname);
const char *http_req_query(const http_request *r, const char *key);
const char *http_status_text(int status);

/* Percent-decode; plus_as_space for query strings. Returns NULL if a NUL byte would result. */
char *http_url_decode(const char *s, size_t n, int plus_as_space);

/* Parse one request from buf. Returns >0 = bytes consumed (request filled),
 * 0 = need more data, <0 = -HTTP status to reply with before closing.
 * Bodies above HTTP_MAX_BODY are borrowed from buf (body_borrowed). */
long http_parse_request(const char *buf, size_t len, http_request *req, int *expect_continue);
/* Same, with a per-request body limit callback (may be NULL). */
long http_parse_request_ex(const char *buf, size_t len, http_request *req, int *expect_continue,
                           http_body_limit_fn limit, void *ctx);
/* Same; when 0 is returned with the headers complete, *need (if non-NULL) is
 * the total byte count the request needs (headers + body). */
long http_parse_request_need(const char *buf, size_t len, http_request *req, int *expect_continue,
                             http_body_limit_fn limit, void *ctx, size_t *need);
void http_request_free(http_request *r);

int http_server_listen(http_server *srv, const char *host, int port);
/* On return (after *stop) the server has drained: no new connections,
 * event streams closed, requests already arriving answered, held responses
 * committed and flushed (bounded by drain_ms). */
/* "kqueue", "epoll" or "poll" (valid once the server runs). */
const char *http_server_poller(const http_server *srv);
/* Runs until *stop becomes non-zero. Uses kqueue (macOS / BSD) or epoll
 * (Linux), poll() elsewhere or with use_poll / -DMODX_POLLER_POLL. */
void http_server_run(http_server *srv, volatile sig_atomic_t *stop);
void http_server_close(http_server *srv);

/* Queue bytes on a connection (SSE / WebSocket pushes). Marks it dead on
 * overflow. With a durability barrier, bytes pushed while changes are
 * pending wait for the commit and go out right after the change
 * notifications it triggers (dropped if the commit fails), so nothing is
 * announced before what it refers to is durable. */
void http_conn_send(http_conn *c, const char *data, size_t len);

#endif
