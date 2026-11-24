#include "modx/http.h"
#include "modx/gzip.h"
#include "modx/ws.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

const char *http_status_text(int s) {
    switch (s) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Unknown";
    }
}

const char *http_req_header(const http_request *r, const char *lname) {
    for (size_t i = 0; i < r->nheaders; i++)
        if (strcmp(r->headers[i].key, lname) == 0) return r->headers[i].val;
    return NULL;
}

const char *http_req_header_last(const http_request *r, const char *lname) {
    const char *hit = NULL;
    for (size_t i = 0; i < r->nheaders; i++)
        if (strcmp(r->headers[i].key, lname) == 0) hit = r->headers[i].val;
    return hit;
}

const char *http_req_query(const http_request *r, const char *key) {
    for (size_t i = 0; i < r->nquery; i++)
        if (strcmp(r->query[i].key, key) == 0) return r->query[i].val;
    return NULL;
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *http_url_decode(const char *s, size_t n, int plus_as_space) {
    sbuf b;
    sbuf_init(&b);
    sbuf_reserve(&b, n);
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '%' && i + 2 < n) {
            int h = hexv(s[i + 1]), l = hexv(s[i + 2]);
            if (h >= 0 && l >= 0) {
                char d = (char)(h * 16 + l);
                if (d == 0) { sbuf_free(&b); return NULL; }
                sbuf_putc(&b, d);
                i += 2;
                continue;
            }
        }
        if (c == '+' && plus_as_space) c = ' ';
        sbuf_putc(&b, c);
    }
    return sbuf_take(&b);
}

static void add_kv(http_kv **arr, size_t *n, char *k, char *v) {
    *arr = xrealloc(*arr, (*n + 1) * sizeof **arr);
    (*arr)[*n].key = k;
    (*arr)[*n].val = v;
    (*n)++;
}

static void parse_query(http_request *r, const char *q) {
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t seg = amp ? (size_t)(amp - q) : strlen(q);
        if (seg) {
            const char *eq = memchr(q, '=', seg);
            size_t klen = eq ? (size_t)(eq - q) : seg;
            char *k = http_url_decode(q, klen, 1);
            char *v = eq ? http_url_decode(eq + 1, seg - klen - 1, 1) : xstrdup("");
            if (k && v) add_kv(&r->query, &r->nquery, k, v);
            else { free(k); free(v); }
        }
        if (!amp) break;
        q = amp + 1;
    }
}

void http_request_free(http_request *r) {
    free(r->target);
    free(r->path);
    for (size_t i = 0; i < r->nquery; i++) { free(r->query[i].key); free(r->query[i].val); }
    free(r->query);
    for (size_t i = 0; i < r->nheaders; i++) { free(r->headers[i].key); free(r->headers[i].val); }
    free(r->headers);
    if (!r->body_borrowed) free(r->body);
    memset(r, 0, sizeof *r);
}

static const char *find_crlfcrlf(const char *buf, size_t len) {
    if (len < 4) return NULL;
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return buf + i;
    return NULL;
}

long http_parse_request(const char *buf, size_t len, http_request *req, int *expect_continue) {
    return http_parse_request_ex(buf, len, req, expect_continue, NULL, NULL);
}

long http_parse_request_ex(const char *buf, size_t len, http_request *req, int *expect_continue,
                           http_body_limit_fn limit_fn, void *limit_ctx) {
    return http_parse_request_need(buf, len, req, expect_continue, limit_fn, limit_ctx, NULL);
}

long http_parse_request_need(const char *buf, size_t len, http_request *req, int *expect_continue,
                             http_body_limit_fn limit_fn, void *limit_ctx, size_t *need) {
    memset(req, 0, sizeof *req);
    if (need) *need = 0;
    *expect_continue = 0;
    const char *end = find_crlfcrlf(buf, len);
    if (!end) return len > HTTP_MAX_HEADER ? -431 : 0;
    size_t head_len = (size_t)(end - buf) + 4;
    if (head_len > HTTP_MAX_HEADER) return -431;

    /* request line */
    const char *line_end = memchr(buf, '\r', head_len);
    const char *sp1 = memchr(buf, ' ', (size_t)(line_end - buf));
    if (!sp1 || sp1 == buf || (size_t)(sp1 - buf) >= sizeof req->method) return -400;
    const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1));
    if (!sp2 || sp2 == sp1 + 1) return -400;
    const char *ver = sp2 + 1;
    size_t ver_len = (size_t)(line_end - ver);
    int http10;
    if (ver_len == 8 && memcmp(ver, "HTTP/1.1", 8) == 0) http10 = 0;
    else if (ver_len == 8 && memcmp(ver, "HTTP/1.0", 8) == 0) http10 = 1;
    else return -400;
    memcpy(req->method, buf, (size_t)(sp1 - buf));
    req->method[sp1 - buf] = 0;
    for (char *m = req->method; *m; m++) if (!isupper((unsigned char)*m)) return -400;
    req->target = xstrndup(sp1 + 1, (size_t)(sp2 - sp1 - 1));
    if (req->target[0] != '/') { http_request_free(req); return -400; }
    char *qm = strchr(req->target, '?');
    size_t plen = qm ? (size_t)(qm - req->target) : strlen(req->target);
    req->path = http_url_decode(req->target, plen, 0);
    if (!req->path) { http_request_free(req); return -400; }
    if (qm) parse_query(req, qm + 1);

    /* headers */
    const char *p = line_end + 2;
    long content_length = 0;
    int have_cl = 0;
    req->keep_alive = !http10;
    while (p < end + 2) {
        const char *le = memchr(p, '\r', (size_t)(end + 2 - p));
        if (!le || le == p) break;
        const char *colon = memchr(p, ':', (size_t)(le - p));
        if (!colon || colon == p) { http_request_free(req); return -400; }
        char *name = xstrndup(p, (size_t)(colon - p));
        for (char *c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
        const char *v = colon + 1;
        while (v < le && (*v == ' ' || *v == '\t')) v++;
        const char *ve = le;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        char *val = xstrndup(v, (size_t)(ve - v));
        add_kv(&req->headers, &req->nheaders, name, val);
        if (strcmp(name, "content-length") == 0) {
            if (!*val || have_cl) { http_request_free(req); return -400; }
            char *e;
            errno = 0;
            long n = strtol(val, &e, 10);
            if (*e || n < 0 || errno) { http_request_free(req); return -400; }
            if (n > HTTP_MAX_UPLOAD) { http_request_free(req); return -413; }
            content_length = n;
            have_cl = 1;
        } else if (strcmp(name, "transfer-encoding") == 0) {
            http_request_free(req);
            return -411;
        } else if (strcmp(name, "connection") == 0) {
            if (str_icase_contains(val, "close")) req->keep_alive = 0;
            else if (str_icase_contains(val, "keep-alive")) req->keep_alive = 1;
        } else if (strcmp(name, "expect") == 0) {
            if (str_icase_eq(val, "100-continue")) *expect_continue = 1;
        }
        p = le + 2;
    }
    if (content_length > HTTP_MAX_BODY) {
        size_t lim = limit_fn ? limit_fn(req, limit_ctx) : HTTP_MAX_BODY;
        if (lim > HTTP_MAX_UPLOAD) lim = HTTP_MAX_UPLOAD;
        if ((size_t)content_length > lim) { http_request_free(req); return -413; }
    }
    if (len < head_len + (size_t)content_length) {
        http_request_free(req);
        if (need) *need = head_len + (size_t)content_length;
        return 0; /* expect_continue stays set so the caller can send 100 */
    }
    *expect_continue = 0;
    if ((size_t)content_length > HTTP_MAX_BODY) {
        /* a raw upload: hand out a view into the input buffer instead of a copy */
        req->body = (char *)(uintptr_t)(buf + head_len);
        req->body_borrowed = 1;
    } else {
        req->body = xstrndup(buf + head_len, (size_t)content_length);
    }
    req->body_len = (size_t)content_length;
    return (long)(head_len + (size_t)content_length);
}

/* ---------------- server ---------------- */

static void poller_add(http_server *srv, http_conn *c);

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int http_server_listen(http_server *srv, const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(fd); errno = EINVAL; return -1; }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 128) != 0 || set_nonblock(fd) != 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    socklen_t al = sizeof addr;
    getsockname(fd, (struct sockaddr *)&addr, &al);
    srv->listen_fd = fd;
    srv->poll_fd = -1;
    srv->port = ntohs(addr.sin_port);
    return 0;
}

void http_conn_send(http_conn *c, const char *data, size_t len) {
    if (c->dead) return;
    if (c->out.len - c->out_off + c->deferred.len + len > HTTP_SSE_MAX_BACKLOG) { c->dead = 1; return; }
    http_server *srv = c->srv;
    if (srv && srv->durable_pending && !srv->in_barrier && srv->durable_pending(srv->durable_ctx)) {
        sbuf_put(&c->deferred, data, len);
        return;
    }
    sbuf_put(&c->out, data, len);
}

static void conn_free(http_server *srv, http_conn *c) {
    close(c->fd);
    if (c->send_fd >= 0) close(c->send_fd);
    if (c->app) { if (srv->app_free) srv->app_free(c->app); else free(c->app); }
    sbuf_free(&c->in);
    sbuf_free(&c->out);
    sbuf_free(&c->ws_msg);
    sbuf_free(&c->deferred);
    free(c);
}

static void write_simple_error(http_conn *c, int status) {
    sbuf body;
    sbuf_init(&body);
    sbuf_printf(&body, "{\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                status >= 500 ? "internal" : status == 413 ? "payload_too_large" : "bad_request",
                http_status_text(status));
    sbuf_printf(&c->out,
                "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                status, http_status_text(status), body.len);
    sbuf_put(&c->out, body.data, body.len);
    sbuf_free(&body);
    c->close_after_write = 1;
}

static int has_header(const sbuf *h, const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; h->data && i + n < h->len; i++)
        if ((i == 0 || h->data[i - 1] == '\n') && strncasecmp(h->data + i, name, n) == 0 && h->data[i + n] == ':') return 1;
    return 0;
}

/* gzip a buffered response when the client takes it and it is worth it. */
static void maybe_gzip(http_server *srv, const http_request *req, http_response *resp) {
    if (!srv->gzip_min || resp->file_fd >= 0 || resp->status != 200 || strcmp(req->method, "HEAD") == 0) return;
    if (!mx_compressible(resp->content_type) || has_header(&resp->headers, "content-encoding")) return;
    sbuf_puts(&resp->headers, "Vary: Accept-Encoding\r\n");
    if (resp->body.len < srv->gzip_min || !mx_accepts_gzip(http_req_header(req, "accept-encoding"))) return;
    sbuf z;
    sbuf_init(&z);
    if (mx_gzip(resp->body.data, resp->body.len, &z, srv->gzip_level) == 0 && z.len < resp->body.len) {
        srv->gzip_in += (int64_t)resp->body.len;
        srv->gzip_out += (int64_t)z.len;
        sbuf_free(&resp->body);
        resp->body = z;
        sbuf_puts(&resp->headers, "Content-Encoding: gzip\r\n");
    } else {
        sbuf_free(&z);
    }
}

static void serialize_response(http_server *srv, http_conn *c, const http_request *req, http_response *resp) {
    if (resp->ws) {
        sbuf_puts(&c->out, "HTTP/1.1 101 Switching Protocols\r\n");
        if (resp->headers.len) sbuf_put(&c->out, resp->headers.data, resp->headers.len);
        sbuf_puts(&c->out, "\r\n");
        c->is_ws = 1;
        return;
    }
    if (resp->sse) {
        /* never gzip a stream: the catch-up body and every later event share one connection, and a
           compressed first chunk followed by raw events breaks the client's decoder */
        sbuf_printf(&c->out,
                    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                    "Connection: keep-alive\r\nX-Accel-Buffering: no\r\n");
        if (resp->headers.len) sbuf_put(&c->out, resp->headers.data, resp->headers.len);
        sbuf_puts(&c->out, "\r\n");
        sbuf_put(&c->out, resp->body.data, resp->body.len);
        c->is_sse = 1;
        return;
    }
    maybe_gzip(srv, req, resp);
    int head = strcmp(req->method, "HEAD") == 0;
    int no_body = head || resp->status == 204 || resp->status == 304;
    int file = resp->file_fd >= 0;
    sbuf_printf(&c->out, "HTTP/1.1 %d %s\r\n", resp->status, http_status_text(resp->status));
    if (resp->content_type && resp->status != 204)
        sbuf_printf(&c->out, "Content-Type: %s\r\n", resp->content_type);
    if (resp->status != 204 && resp->status != 304)
        sbuf_printf(&c->out, "Content-Length: %zu\r\n", file ? resp->file_len : resp->body.len);
    int keep = req->keep_alive && !srv->draining;
    sbuf_printf(&c->out, "Connection: %s\r\n", keep ? "keep-alive" : "close");
    if (resp->headers.len) sbuf_put(&c->out, resp->headers.data, resp->headers.len);
    sbuf_puts(&c->out, "\r\n");
    if (file) {
        /* streamed from conn_flush once the headers are out: at most one chunk in memory */
        if (no_body || resp->file_len == 0) close(resp->file_fd);
        else { c->send_fd = resp->file_fd; c->send_left = resp->file_len; }
        resp->file_fd = -1;
    } else if (!no_body) {
        sbuf_put(&c->out, resp->body.data, resp->body.len);
    }
    if (!keep) c->close_after_write = 1;
}

static void process_input(http_server *srv, http_conn *c) {
    while (!c->is_sse && !c->is_ws && !c->close_after_write && c->in.len) {
        if (c->send_fd >= 0) { c->resume = 1; return; } /* keep responses in order */
        /* body still arriving: the headers were parsed (and the body limit checked) once */
        if (c->need && c->in.len < c->need) return;
        http_request req;
        int expect;
        size_t need = 0;
        long n = http_parse_request_need(c->in.data, c->in.len, &req, &expect, srv->body_limit, srv->ctx, &need);
        if (n == 0) {
            if (expect && !c->sent_continue) {
                sbuf_puts(&c->out, "HTTP/1.1 100 Continue\r\n\r\n");
                c->sent_continue = 1;
            }
            if (need) {
                /* one exactly-sized buffer for the whole request (no doubling to 64 MiB) */
                c->need = need;
                sbuf_reserve_exact(&c->in, need - c->in.len);
            }
            return;
        }
        c->need = 0;
        if (n < 0) {
            write_simple_error(c, (int)-n);
            c->in.len = 0;
            return;
        }
        c->sent_continue = 0;
        http_response resp;
        memset(&resp, 0, sizeof resp);
        resp.status = 200;
        resp.file_fd = -1;
        resp.content_type = "application/json; charset=utf-8";
        sbuf_init(&resp.body);
        sbuf_init(&resp.headers);
        srv->handler(srv, c, &req, &resp, srv->ctx);
        if (srv->durable_pending && !resp.sse && srv->durable_pending(srv->durable_ctx) && !c->held) {
            c->held = 1;
            c->hold_mark = c->out.len;
        }
        serialize_response(srv, c, &req, &resp);
        if (resp.file_fd >= 0) close(resp.file_fd);
        sbuf_free(&resp.body);
        sbuf_free(&resp.headers);
        http_request_free(&req);
        sbuf_consume(&c->in, (size_t)n);
        c->req_start = c->in.len ? mono_ms() : 0; /* a pipelined request may already be arriving */
        c->requests++;
        if (srv->draining) c->close_after_write = 1;
        /* don't keep a 50 MiB upload buffer around on an idle keep-alive connection */
        if (c->in.cap > HTTP_MAX_HEADER + HTTP_MAX_BODY) sbuf_shrink(&c->in);
    }
    if (c->is_ws && c->in.len && !c->close_after_write) mx_ws_on_input(srv, c);
}

static int conn_pending(const http_conn *c) { return c->out.len > c->out_off || c->send_fd >= 0; }

static void conn_flush(http_conn *c) {
    if (c->held) return; /* waits for the durability barrier */
    for (;;) {
        while (c->out_off < c->out.len) {
            ssize_t w = write(c->fd, c->out.data + c->out_off, c->out.len - c->out_off);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                c->dead = 1;
                return;
            }
            c->out_off += (size_t)w;
            c->out_since = mono_ms(); /* progress */
        }
        c->out.len = 0;
        c->out_off = 0;
        if (c->out.data) c->out.data[0] = 0;
        if (c->send_fd < 0) break;
        if (c->send_left == 0) { close(c->send_fd); c->send_fd = -1; c->last_active = mono_ms(); break; }
        /* next chunk of a streamed file response */
        size_t chunk = c->send_left < HTTP_FILE_CHUNK ? c->send_left : HTTP_FILE_CHUNK;
        sbuf_reserve(&c->out, chunk);
        ssize_t r = read(c->send_fd, c->out.data, chunk);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { c->dead = 1; return; } /* file shrank or vanished: the length is promised, drop the connection */
        c->out.len = (size_t)r;
        c->out.data[c->out.len] = 0;
        c->send_left -= (size_t)r;
        c->last_active = mono_ms();
    }
    if (c->close_after_write && !c->linger_until) {
        /* lingering close: stop sending, keep reading (and discarding) for a moment, so a peer that is still
         * sending (an oversized body, a WebSocket frame) reads our answer instead of a reset */
        if (shutdown(c->fd, SHUT_WR) == 0) c->linger_until = mono_ms() + HTTP_LINGER_MS;
        else c->dead = 1;
    }
}

static void conn_read(http_server *srv, http_conn *c) {
    char scratch[16384];
    size_t budget = HTTP_READ_BUDGET; /* fairness: one large upload doesn't monopolize the loop */
    for (;;) {
        int discard = c->is_sse || c->close_after_write; /* ignore input on streams */
        char *dst = scratch;
        size_t want = sizeof scratch;
        if (!discard) {
            want = 65536;
            /* never read past the request being received: its buffer was sized exactly */
            if (c->need > c->in.len && c->need - c->in.len < want) want = c->need - c->in.len;
            sbuf_reserve(&c->in, want);
            dst = c->in.data + c->in.len; /* read straight into the input buffer */
        }
        ssize_t n = read(c->fd, dst, want);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            c->dead = 1;
            return;
        }
        if (n == 0) { c->dead = 1; return; }
        c->last_active = mono_ms();
        if (c->linger_until || discard) { /* discarded, but still within the read budget */
            if ((size_t)n >= budget) break;
            budget -= (size_t)n;
            continue;
        }
        if (!c->req_start) c->req_start = c->last_active;
        c->in.len += (size_t)n;
        c->in.data[c->in.len] = 0;
        if ((size_t)n >= budget) break;
        budget -= (size_t)n;
        if (c->need ? c->in.len >= c->need : c->in.len > HTTP_MAX_HEADER + HTTP_MAX_BODY + 4096)
            break; /* parse before buffering more */
    }
    process_input(srv, c);
}

static void accept_all(http_server *srv) {
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t plen = sizeof peer;
        int fd = accept(srv->listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) return;
        if (srv->nconns >= (srv->max_conns ? srv->max_conns : HTTP_MAX_CONNS) || set_nonblock(fd) != 0) { close(fd); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        http_conn *c = xcalloc(1, sizeof *c);
        c->fd = fd;
        if (peer.ss_family == AF_INET)
            inet_ntop(AF_INET, &((struct sockaddr_in *)&peer)->sin_addr, c->ip, sizeof c->ip);
        else if (peer.ss_family == AF_INET6)
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&peer)->sin6_addr, c->ip, sizeof c->ip);
        sbuf_init(&c->in);
        sbuf_init(&c->out);
        c->send_fd = -1;
        c->last_active = mono_ms();
        c->srv = srv;
        srv->conns = xrealloc(srv->conns, (srv->nconns + 1) * sizeof *srv->conns);
        srv->conns[srv->nconns++] = c;
        if (srv->nconns > srv->nconns_peak) srv->nconns_peak = srv->nconns;
        if (srv->running) poller_add(srv, c);
    }
}

static int64_t or_default(int64_t v, int64_t d) { return v ? v : d; }

/* Slow or stuck peers: headers that never finish (slowloris), bodies that
 * trickle, keep-alive connections that idle, output nobody reads. */
static void timeouts(http_server *srv, int64_t now) {
    int64_t hdr = or_default(srv->header_timeout_ms, HTTP_HEADER_TIMEOUT_MS);
    int64_t body = or_default(srv->body_timeout_ms, HTTP_BODY_TIMEOUT_MS);
    int64_t wr = or_default(srv->write_timeout_ms, HTTP_WRITE_TIMEOUT_MS);
    int64_t idle = or_default(srv->idle_ms, HTTP_IDLE_MS);
    for (size_t i = 0; i < srv->nconns; i++) {
        http_conn *c = srv->conns[i];
        if (c->dead) continue;
        int pending = conn_pending(c) && !c->held;
        if (pending && c->out_since && now - c->out_since > wr) { c->dead = 1; srv->timeouts++; continue; }
        if (c->linger_until) { if (now > c->linger_until) c->dead = 1; continue; }
        if (c->is_ws) { mx_ws_tick(srv, c, now); continue; }
        if (c->is_sse || c->close_after_write) continue;
        if (c->req_start && !c->held && !pending) {
            int64_t limit = c->need ? body : hdr; /* need is set once the headers are parsed */
            if (now - c->req_start > limit) {
                static const char msg[] = "HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                ssize_t w = write(c->fd, msg, sizeof msg - 1);
                (void)w;
                c->dead = 1;
                srv->timeouts++;
                continue;
            }
        }
        if (!c->req_start && !pending && now - c->last_active > idle) c->dead = 1;
    }
}

static void reap(http_server *srv) {
    int64_t now = mono_ms();
    if (now - srv->last_timeout_check >= 100) {
        srv->last_timeout_check = now;
        timeouts(srv, now);
    }
    size_t k = 0;
    /* close hooks first, while every connection pointer is still valid */
    for (size_t i = 0; i < srv->nconns; i++) {
        http_conn *c = srv->conns[i];
        if (c->dead && c->app && srv->on_close) srv->on_close(srv, c, srv->ctx);
    }
    for (size_t i = 0; i < srv->nconns; i++) {
        http_conn *c = srv->conns[i];
        if (c->dead) conn_free(srv, c);
        else srv->conns[k++] = c;
    }
    srv->nconns = k;
}

/* Group commit: make this iteration's changes durable, then release (or, on
 * failure, replace) every held response. */
static void barrier(http_server *srv) {
    int held = 0;
    for (size_t i = 0; i < srv->nconns && !held; i++) held = srv->conns[i]->held;
    if (!srv->durable_commit) return;
    int ok = 1, unavailable = 0;
    if (srv->durable_pending && srv->durable_pending(srv->durable_ctx)) {
        srv->commits++;
        srv->in_barrier = 1; /* notifications sent by the commit go straight out */
        int rc = srv->durable_commit(srv->durable_ctx);
        srv->in_barrier = 0;
        ok = rc == 0;
        unavailable = rc <= HTTP_DURABLE_UNAVAILABLE;
        if (!ok) srv->commit_failures++;
    }
    /* pushes queued during the iteration follow the change notifications */
    for (size_t i = 0; i < srv->nconns; i++) {
        http_conn *c = srv->conns[i];
        if (!c->deferred.len) continue;
        if (ok && !c->dead) sbuf_put(&c->out, c->deferred.data, c->deferred.len);
        c->deferred.len = 0;
        if (c->deferred.cap > 65536) sbuf_free(&c->deferred);
    }
    if (!held) return;
    for (size_t i = 0; i < srv->nconns; i++) {
        http_conn *c = srv->conns[i];
        if (!c->held) continue;
        c->held = 0;
        if (!ok) {
            /* nothing of what these responses report is durable */
            c->out.len = c->hold_mark;
            if (c->out.data) c->out.data[c->out.len] = 0;
            if (c->send_fd >= 0) { close(c->send_fd); c->send_fd = -1; c->send_left = 0; }
            static const char body[] = "{\"error\":{\"code\":\"internal\",\"message\":\"storage write failed\"}}";
            static const char body503[] =
                "{\"error\":{\"code\":\"unavailable\",\"message\":\"storage is read-only after an I/O error\"}}";
            if (unavailable)
                sbuf_printf(&c->out,
                            "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json; charset=utf-8\r\n"
                            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", sizeof body503 - 1, body503);
            else
                sbuf_printf(&c->out,
                            "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json; charset=utf-8\r\n"
                            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s", sizeof body - 1, body);
            c->close_after_write = 1;
            c->in.len = 0;
        }
    }
}

/* ---------------- pollers ----------------
 * kqueue (macOS / BSD) and epoll (Linux) register each socket once and
 * return only ready ones; poll() rebuilds the whole set every iteration and
 * is kept as the portable fallback (and for tests: http_server.use_poll). */

#if defined(MODX_POLLER_POLL)
#define HAVE_KQUEUE 0
#define HAVE_EPOLL 0
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#define HAVE_KQUEUE 1
#define HAVE_EPOLL 0
#include <sys/event.h>
#elif defined(__linux__)
#define HAVE_KQUEUE 0
#define HAVE_EPOLL 1
#include <sys/epoll.h>
#else
#define HAVE_KQUEUE 0
#define HAVE_EPOLL 0
#endif

const char *http_server_poller(const http_server *srv) {
    if (srv->use_poll || srv->poll_fd < 0) return "poll";
    return HAVE_KQUEUE ? "kqueue" : HAVE_EPOLL ? "epoll" : "poll";
}

static int poller_open(http_server *srv) {
    srv->poll_fd = -1;
    if (srv->use_poll) return 0;
#if HAVE_KQUEUE
    srv->poll_fd = kqueue();
    if (srv->poll_fd < 0) return 0; /* fall back to poll() */
    struct kevent ev;
    EV_SET(&ev, srv->listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(srv->poll_fd, &ev, 1, NULL, 0, NULL) != 0) { close(srv->poll_fd); srv->poll_fd = -1; }
#elif HAVE_EPOLL
    srv->poll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (srv->poll_fd < 0) return 0;
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(srv->poll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev) != 0) { close(srv->poll_fd); srv->poll_fd = -1; }
#endif
    return 0;
}

static void poller_add(http_server *srv, http_conn *c) {
    if (srv->poll_fd < 0) return;
#if HAVE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, c->fd, EVFILT_READ, EV_ADD, 0, 0, c);
    if (kevent(srv->poll_fd, &ev, 1, NULL, 0, NULL) != 0) c->dead = 1;
#elif HAVE_EPOLL
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.ptr = c;
    if (epoll_ctl(srv->poll_fd, EPOLL_CTL_ADD, c->fd, &ev) != 0) c->dead = 1;
#else
    (void)c;
#endif
}

/* Ask for writability only while output is waiting (no busy wake-ups). */
static void poller_want_write(http_server *srv, http_conn *c, int want) {
    if (srv->poll_fd < 0 || c->want_write == want || c->dead) return;
    c->want_write = want;
#if HAVE_KQUEUE
    struct kevent ev;
    EV_SET(&ev, c->fd, EVFILT_WRITE, want ? EV_ADD : EV_DELETE, 0, 0, c);
    kevent(srv->poll_fd, &ev, 1, NULL, 0, NULL);
#elif HAVE_EPOLL
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN | EPOLLRDHUP | (want ? EPOLLOUT : 0);
    ev.data.ptr = c;
    epoll_ctl(srv->poll_fd, EPOLL_CTL_MOD, c->fd, &ev);
#endif
}

#define POLLER_BATCH 256

/* Wait for events (up to timeout_ms) and handle them. */
static void wait_events(http_server *srv, int timeout_ms, volatile sig_atomic_t *stop) {
#if HAVE_KQUEUE
    if (srv->poll_fd >= 0) {
        struct kevent evs[POLLER_BATCH];
        struct timespec ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
        int n = kevent(srv->poll_fd, NULL, 0, evs, POLLER_BATCH, &ts);
        if (n < 0 && errno != EINTR) { perror("kevent"); *stop = 1; return; }
        if (*stop) return;
        int accept_ready = 0;
        for (int i = 0; i < n; i++) {
            http_conn *c = evs[i].udata;
            if (!c) { accept_ready = 1; continue; }
            if (c->dead) continue;
            if (evs[i].filter == EVFILT_READ) conn_read(srv, c);
            else if (evs[i].filter == EVFILT_WRITE) conn_flush(c);
        }
        if (accept_ready) accept_all(srv);
        return;
    }
#elif HAVE_EPOLL
    if (srv->poll_fd >= 0) {
        struct epoll_event evs[POLLER_BATCH];
        int n = epoll_wait(srv->poll_fd, evs, POLLER_BATCH, timeout_ms);
        if (n < 0 && errno != EINTR) { perror("epoll_wait"); *stop = 1; return; }
        if (*stop) return;
        int accept_ready = 0;
        for (int i = 0; i < n; i++) {
            http_conn *c = evs[i].data.ptr;
            if (!c) { accept_ready = 1; continue; }
            if (c->dead) continue;
            if (evs[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP)) conn_read(srv, c);
            if (!c->dead && (evs[i].events & EPOLLOUT)) conn_flush(c);
        }
        if (accept_ready) accept_all(srv);
        return;
    }
#endif
    /* poll(): one pollfd per connection, rebuilt every iteration */
    size_t n = srv->nconns + 1;
    if (n > srv->pcap) { srv->pcap = n * 2; srv->pfds = xrealloc(srv->pfds, srv->pcap * sizeof *srv->pfds); }
    struct pollfd *pfds = srv->pfds;
    pfds[0].fd = srv->listen_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    for (size_t i = 0; i < srv->nconns; i++) {
        http_conn *c = srv->conns[i];
        pfds[i + 1].fd = c->fd;
        pfds[i + 1].events = POLLIN | (conn_pending(c) && !c->held ? POLLOUT : 0);
        pfds[i + 1].revents = 0;
    }
    int rc = poll(pfds, (nfds_t)n, timeout_ms);
    if (rc < 0 && errno != EINTR) { perror("poll"); *stop = 1; return; }
    if (*stop || rc <= 0) return;
    /* conns may grow during this pass (accept), so handle existing ones first */
    size_t existing = srv->nconns;
    for (size_t i = 0; i < existing; i++) {
        http_conn *c = srv->conns[i];
        short re = pfds[i + 1].revents;
        if (re & (POLLIN | POLLHUP | POLLERR)) conn_read(srv, c);
        if (!c->dead && (re & POLLOUT)) conn_flush(c);
    }
    if (pfds[0].revents & POLLIN) accept_all(srv);
}

static void loop_once(http_server *srv, int timeout_ms, volatile sig_atomic_t *stop);

/* Graceful shutdown: stop accepting, end event streams, let requests that
 * are already arriving finish (their responses carry Connection: close),
 * make everything durable, flush, then return. Bounded by drain_ms. */
static void drain(http_server *srv) {
    srv->draining = 1;
    if (srv->listen_fd >= 0) { close(srv->listen_fd); srv->listen_fd = -1; }
    int64_t deadline = mono_ms() + or_default(srv->drain_ms, HTTP_DRAIN_MS);
    volatile sig_atomic_t never = 0;
    for (;;) {
        size_t busy = 0;
        for (size_t i = 0; i < srv->nconns; i++) {
            http_conn *c = srv->conns[i];
            if (c->dead) continue;
            if (c->is_sse) { c->dead = 1; continue; } /* clients reconnect elsewhere / later */
            if (c->is_ws && !c->ws_closing) mx_ws_close(c, 1001, "server shutting down");
            if (c->req_start || conn_pending(c) || c->held) busy++;
            else c->dead = 1;                         /* idle keep-alive: just close */
        }
        if (!busy || mono_ms() > deadline) break;
        loop_once(srv, 50, &never);
    }
    barrier(srv);
    reap(srv);
}

void http_server_run(http_server *srv, volatile sig_atomic_t *stop) {
    poller_open(srv);
    for (size_t i = 0; i < srv->nconns; i++) poller_add(srv, srv->conns[i]);
    srv->running = 1;
    while (!*stop) loop_once(srv, 1000, stop);
    drain(srv);
    srv->running = 0;
}

static void loop_once(http_server *srv, int timeout_ms, volatile sig_atomic_t *stop) {
    {
        wait_events(srv, timeout_ms, stop);
        if (*stop) return;
        if (srv->tick) srv->tick(srv, srv->ctx);
        barrier(srv);
        /* flush everything queued this round (responses, SSE pushes) */
        for (size_t i = 0; i < srv->nconns; i++) {
            http_conn *c = srv->conns[i];
            if (!c->dead && conn_pending(c)) conn_flush(c);
            /* a streamed file finished: handle requests pipelined behind it */
            if (!c->dead && c->resume && c->send_fd < 0) {
                c->resume = 0;
                process_input(srv, c);
                if (!c->dead && conn_pending(c)) conn_flush(c);
            }
            int pending = !c->dead && conn_pending(c) && !c->held;
            if (!pending) c->out_since = 0;
            else if (!c->out_since) c->out_since = mono_ms();
            poller_want_write(srv, c, pending);
        }
        reap(srv);
    }
}

void http_server_close(http_server *srv) {
    barrier(srv); /* held responses go out only once their changes are durable */
    for (size_t i = 0; i < srv->nconns; i++) {
        http_conn *c = srv->conns[i];
        /* best-effort, non-blocking flush of pending output */
        if (c->out.len > c->out_off && !c->dead) {
            ssize_t w = write(c->fd, c->out.data + c->out_off, c->out.len - c->out_off);
            (void)w;
        }
        conn_free(srv, c);
    }
    free(srv->conns);
    srv->conns = NULL;
    srv->nconns = 0;
    if (srv->listen_fd >= 0) close(srv->listen_fd);
    srv->listen_fd = -1;
    if (srv->poll_fd >= 0) close(srv->poll_fd);
    srv->poll_fd = -1;
    free(srv->pfds);
    srv->pfds = NULL;
    srv->pcap = 0;
}
