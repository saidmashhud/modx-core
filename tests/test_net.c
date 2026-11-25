/* The server over real sockets, in-process: the event loop runs on its own
 * thread, the test is the client. Deferred responses with async password
 * verification (the loop keeps serving while a worker hashes; pipelined
 * answers stay in order; a client that leaves early; the durability
 * barrier), and the listen addresses (IPv4, IPv6, dual stack). */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "modx/crypto.h"
#include "modx/http.h"
#include "modx/pool.h"
#include "modx/session.h"
#include "modx/store.h"
#include "test.h"
#include "test_util.h"

/* ---------------- the server under test ---------------- */

typedef struct {
    http_server srv;
    mx_pool *pool;
    mx_store *st;
    char *hash;                 /* stored password hash */
    volatile sig_atomic_t stop;
    pthread_t thread;
    int64_t verify_ms;          /* how long one verification took on a worker (set on the loop thread) */
} tserver;

typedef struct {
    tserver *ts;
    http_deferred *d;
    int64_t started;
} login_ctx;

static void login_done(void *ctx, int ok, int rehash) {
    (void)rehash;
    login_ctx *l = ctx;
    __atomic_store_n(&l->ts->verify_ms, mono_ms() - l->started, __ATOMIC_SEQ_CST);
    http_response *r = http_deferred_response(l->d);
    if (ok && l->ts->st) {
        /* a session row: the answer must wait until it is durable */
        json *row = json_obj();
        json_set(row, "id", json_str("session-1"));
        mx_store_put(l->ts->st, 0, row, NULL);
    }
    r->status = ok ? 200 : 401;
    sbuf_puts(&r->body, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    if (l->ts->st) sbuf_printf(&r->headers, "X-Durable: %d\r\n", mx_store_dirty(l->ts->st) ? 0 : 1);
    http_deferred_finish(l->d);
    free(l);
}

static void handler(http_server *srv, http_conn *c, const http_request *req, http_response *resp, void *ctx) {
    (void)srv;
    tserver *ts = ctx;
    if (strcmp(req->path, "/login") == 0) {
        login_ctx *l = xcalloc(1, sizeof *l);
        l->ts = ts;
        l->started = mono_ms();
        l->d = http_defer(c, req, resp);
        if (password_verify_async(ts->pool, req->body ? req->body : "", ts->hash, login_done, l) != 0) {
            http_response *r = http_deferred_response(l->d);
            r->status = 503;
            http_deferred_finish(l->d);
            free(l);
        }
        return;
    }
    if (strcmp(req->path, "/quit") == 0) { ts->stop = 1; sbuf_puts(&resp->body, "bye"); return; }
    if (strcmp(req->path, "/ip") == 0) { resp->content_type = "text/plain"; sbuf_puts(&resp->body, c->ip); return; }
    resp->content_type = "text/plain";
    sbuf_puts(&resp->body, "pong");
}

static void *server_main(void *vp) {
    tserver *ts = vp;
    http_server_run(&ts->srv, &ts->stop);
    return NULL;
}

/* Listens on host; 0, or -1 (errno from the listen). */
static int tserver_start(tserver *ts, const char *host, mx_store *st, int use_poll) {
    memset(ts, 0, sizeof *ts);
    ts->srv.listen_fd = -1;
    if (http_server_listen(&ts->srv, host, 0) != 0) return -1;
    ts->srv.handler = handler;
    ts->srv.ctx = ts;
    ts->srv.use_poll = use_poll;
    ts->st = st;
    if (st) {
        ts->srv.durable_pending = mx_store_pending_cb;
        ts->srv.durable_commit = mx_store_commit_cb;
        ts->srv.durable_ctx = st;
    }
    ts->pool = mx_pool_new(2, 0);
    ts->hash = password_hash_iters("secret", 200000); /* ~0.1-0.5 s per verification */
    mx_pool_attach(ts->pool, &ts->srv);
    pthread_create(&ts->thread, NULL, server_main, ts);
    return 0;
}

/* ---------------- client side ---------------- */

static int connect_to(int family, const char *addr, int port) {
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { 15, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int rc;
    if (family == AF_INET6) {
        struct sockaddr_in6 a;
        memset(&a, 0, sizeof a);
        a.sin6_family = AF_INET6;
        a.sin6_port = htons((uint16_t)port);
        inet_pton(AF_INET6, addr, &a.sin6_addr);
        rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    } else {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, addr, &a.sin_addr);
        rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    }
    if (rc != 0) { close(fd); return -1; }
    return fd;
}

static void send_str(int fd, const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

/* Reads one response (headers + Content-Length body) into out (cleared first); keeps
 * bytes of the next one in *rest. Returns the status, or -1. */
static int read_response(int fd, sbuf *rest, sbuf *out) {
    out->len = 0;
    if (out->data) out->data[0] = 0;
    for (;;) {
        char *he = rest->data ? strstr(rest->data, "\r\n\r\n") : NULL;
        if (he) {
            size_t hl = (size_t)(he - rest->data) + 4;
            const char *cl = strstr(rest->data, "Content-Length: ");
            size_t bl = cl && cl < he ? (size_t)atol(cl + 16) : 0;
            if (rest->len >= hl + bl) {
                sbuf_put(out, rest->data, hl + bl);
                sbuf_consume(rest, hl + bl);
                return atoi(out->data + 9);
            }
        }
        char buf[4096];
        ssize_t r = read(fd, buf, sizeof buf);
        if (r <= 0) return -1;
        sbuf_put(rest, buf, (size_t)r);
    }
}

static int request(int family, const char *addr, int port, const char *raw, sbuf *out) {
    int fd = connect_to(family, addr, port);
    if (fd < 0) return -1;
    send_str(fd, raw);
    sbuf rest;
    sbuf_init(&rest);
    int st = read_response(fd, &rest, out);
    sbuf_free(&rest);
    close(fd);
    return st;
}

static void tserver_stop(tserver *ts, int family, const char *addr) {
    sbuf out;
    sbuf_init(&out);
    request(family, addr, ts->srv.port, "GET /quit HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", &out);
    sbuf_free(&out);
    pthread_join(ts->thread, NULL);
    http_server_close(&ts->srv);
    mx_pool_free(ts->pool);
    free(ts->hash);
}

static const char LOGIN_OK[] = "POST /login HTTP/1.1\r\nHost: t\r\nContent-Length: 6\r\n\r\nsecret";
static const char LOGIN_BAD[] = "POST /login HTTP/1.1\r\nHost: t\r\nContent-Length: 6\r\n\r\nsecreT";
static const char PING[] = "GET /ping HTTP/1.1\r\nHost: t\r\n\r\n";

/* ---------------- tests ---------------- */

/* While a worker verifies a password the loop answers other connections. */
static void t_async_login_does_not_block(void) {
    tserver ts;
    if (tserver_start(&ts, "127.0.0.1", NULL, 0) != 0) { CHECK(!"listen"); return; }
    int a = connect_to(AF_INET, "127.0.0.1", ts.srv.port);
    CHECK(a >= 0);
    int64_t t0 = mono_ms();
    send_str(a, LOGIN_OK);
    usleep(10000); /* the login is on a worker now */
    sbuf out, rest;
    sbuf_init(&out);
    sbuf_init(&rest);
    int pings = 0;
    int64_t ping_ms = 0;
    for (int i = 0; i < 5; i++) {
        int64_t p0 = mono_ms();
        if (request(AF_INET, "127.0.0.1", ts.srv.port, PING, &out) == 200 && strstr(out.data, "pong")) pings++;
        int64_t d = mono_ms() - p0;
        if (d > ping_ms) ping_ms = d;
    }
    int st = read_response(a, &rest, &out);
    int64_t login_ms = mono_ms() - t0;
    CHECK(st == 200 && strstr(out.data, "{\"ok\":true}"));
    CHECK(pings == 5);
    /* every ping was answered while the verification ran, far faster than it */
    CHECK(ping_ms * 4 < login_ms);
    if (!(ping_ms * 4 < login_ms)) fprintf(stderr, "  slowest ping %lld ms, login %lld ms\n", (long long)ping_ms, (long long)login_ms);
    /* a wrong password, on the same keep-alive connection */
    send_str(a, LOGIN_BAD);
    CHECK(read_response(a, &rest, &out) == 401);
    close(a);
    sbuf_free(&out);
    sbuf_free(&rest);
    tserver_stop(&ts, AF_INET, "127.0.0.1");
    CHECK(ts.srv.deferred_started == 2 && ts.srv.deferred_finished == 2);
}

/* Requests pipelined behind a deferred one wait for it: answers stay in order. */
static void t_deferred_pipelined_order(void) {
    for (int use_poll = 0; use_poll <= 1; use_poll++) {
        tserver ts;
        if (tserver_start(&ts, "127.0.0.1", NULL, use_poll) != 0) { CHECK(!"listen"); return; }
        int a = connect_to(AF_INET, "127.0.0.1", ts.srv.port);
        sbuf all;
        sbuf_init(&all);
        sbuf_puts(&all, LOGIN_OK);
        sbuf_puts(&all, PING);
        sbuf_puts(&all, LOGIN_BAD);
        sbuf_puts(&all, PING);
        send_str(a, all.data);
        sbuf out, rest;
        sbuf_init(&out);
        sbuf_init(&rest);
        CHECK(read_response(a, &rest, &out) == 200 && strstr(out.data, "{\"ok\":true}"));
        CHECK(read_response(a, &rest, &out) == 200 && strstr(out.data, "pong"));
        CHECK(read_response(a, &rest, &out) == 401);
        CHECK(read_response(a, &rest, &out) == 200 && strstr(out.data, "pong"));
        close(a);
        sbuf_free(&out);
        sbuf_free(&rest);
        sbuf_free(&all);
        tserver_stop(&ts, AF_INET, "127.0.0.1");
    }
}

/* The client hangs up before the worker is done: the late finish goes nowhere. */
static void t_deferred_client_gone(void) {
    tserver ts;
    if (tserver_start(&ts, "127.0.0.1", NULL, 0) != 0) { CHECK(!"listen"); return; }
    int a = connect_to(AF_INET, "127.0.0.1", ts.srv.port);
    send_str(a, LOGIN_OK);
    usleep(20000);
    close(a);
    usleep(20000);
    sbuf out;
    sbuf_init(&out);
    /* wait until the verification has finished, then the server must still serve */
    for (int i = 0; i < 300 && __atomic_load_n(&ts.verify_ms, __ATOMIC_SEQ_CST) == 0; i++) usleep(10000);
    CHECK(request(AF_INET, "127.0.0.1", ts.srv.port, PING, &out) == 200);
    sbuf_free(&out);
    tserver_stop(&ts, AF_INET, "127.0.0.1");
    CHECK(ts.srv.deferred_started == 1 && ts.srv.deferred_finished == 1 && ts.srv.deferred_orphaned == 1);
}

/* A change made by the completion is durable before the deferred answer goes out. */
static void t_deferred_durable_barrier(void) {
    static const mx_coll_def defs[] = { { "sessions", MX_COLL_PRIVATE, NULL } };
    char *dir = make_tmpdir();
    char err[256];
    mx_store *st = mx_store_open(dir, defs, 1, 16, NULL, err, sizeof err);
    CHECK(st != NULL);
    if (!st) { rm_rf(dir); return; }
    tserver ts;
    if (tserver_start(&ts, "127.0.0.1", st, 0) != 0) { CHECK(!"listen"); mx_store_close(st); rm_rf(dir); return; }
    sbuf out;
    sbuf_init(&out);
    CHECK(request(AF_INET, "127.0.0.1", ts.srv.port, LOGIN_OK, &out) == 200);
    /* X-Durable: 0 = it was built while the row was pending; it was held for the barrier */
    CHECK(strstr(out.data, "X-Durable: 0") != NULL);
    sbuf_free(&out);
    tserver_stop(&ts, AF_INET, "127.0.0.1");
    CHECK(ts.srv.commits >= 1 && st->fsyncs >= 1 && st->synced_seq == 1);
    mx_store_close(st);
    st = mx_store_open(dir, defs, 1, 16, NULL, err, sizeof err);
    CHECK(st && mx_store_get(st, 0, "session-1"));
    mx_store_close(st);
    rm_rf(dir);
}

/* GET /ip over family/addr against a server listening on host: the http_conn.ip it saw, or NULL. */
static char *ip_seen(int family, const char *addr, int port) {
    sbuf out;
    sbuf_init(&out);
    char *ip = NULL;
    if (request(family, addr, port, "GET /ip HTTP/1.1\r\nHost: t\r\n\r\n", &out) == 200) {
        char *b = strstr(out.data, "\r\n\r\n");
        if (b) ip = xstrdup(b + 4);
    }
    sbuf_free(&out);
    return ip;
}

static int host_has_ipv6_loopback(void) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    int ok = bind(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

static void t_listen_ipv4(void) {
    tserver ts;
    CHECK(tserver_start(&ts, "127.0.0.1", NULL, 0) == 0);
    CHECK(ts.srv.listen_family == AF_INET && ts.srv.port > 0 && !ts.srv.listen_dual_stack);
    char *ip = ip_seen(AF_INET, "127.0.0.1", ts.srv.port);
    CHECK(ip && strcmp(ip, "127.0.0.1") == 0);
    free(ip);
    CHECK(connect_to(AF_INET6, "::1", ts.srv.port) < 0); /* IPv4 only */
    tserver_stop(&ts, AF_INET, "127.0.0.1");
    /* bad hosts */
    http_server bad;
    memset(&bad, 0, sizeof bad);
    errno = 0;
    CHECK(http_server_listen(&bad, "300.1.2.3", 0) == -1);
    CHECK(http_server_listen(&bad, "127.0.0.1", 70000) == -1 && errno == EINVAL);
    CHECK(http_server_listen(&bad, "[::1", 0) == -1);
}

static void t_listen_ipv6(void) {
    if (!host_has_ipv6_loopback()) {
        fprintf(stderr, "  (no IPv6 loopback on this host: IPv6 listen tests skipped)\n");
        return;
    }
    const char *hosts[] = { "::1", "[::1]" };
    for (int i = 0; i < 2; i++) {
        tserver ts;
        CHECK(tserver_start(&ts, hosts[i], NULL, i) == 0);
        CHECK(ts.srv.listen_family == AF_INET6 && !ts.srv.listen_dual_stack);
        char *ip = ip_seen(AF_INET6, "::1", ts.srv.port);
        CHECK(ip && strcmp(ip, "::1") == 0);
        free(ip);
        /* a deferred answer over IPv6 too */
        sbuf out;
        sbuf_init(&out);
        CHECK(request(AF_INET6, "::1", ts.srv.port, LOGIN_OK, &out) == 200);
        sbuf_free(&out);
        CHECK(connect_to(AF_INET, "127.0.0.1", ts.srv.port) < 0); /* IPv6 only */
        tserver_stop(&ts, AF_INET6, "::1");
    }
    /* "::" (and "" / "*"): dual stack; an IPv4 client shows as IPv4, not ::ffff:127.0.0.1 */
    const char *any[] = { "::", "*" };
    for (int i = 0; i < 2; i++) {
        tserver ts;
        CHECK(tserver_start(&ts, any[i], NULL, 0) == 0);
        CHECK(ts.srv.listen_family == AF_INET6);
        char *v6 = ip_seen(AF_INET6, "::1", ts.srv.port);
        CHECK(v6 && strcmp(v6, "::1") == 0);
        free(v6);
        if (ts.srv.listen_dual_stack) {
            char *v4 = ip_seen(AF_INET, "127.0.0.1", ts.srv.port);
            CHECK(v4 && strcmp(v4, "127.0.0.1") == 0);
            free(v4);
        } else {
            fprintf(stderr, "  (IPV6_V6ONLY is forced on this host: dual-stack check skipped)\n");
        }
        tserver_stop(&ts, AF_INET6, "::1");
    }
    /* "0.0.0.0" stays IPv4 */
    tserver ts;
    CHECK(tserver_start(&ts, "0.0.0.0", NULL, 0) == 0);
    CHECK(ts.srv.listen_family == AF_INET);
    CHECK(connect_to(AF_INET6, "::1", ts.srv.port) < 0);
    tserver_stop(&ts, AF_INET, "127.0.0.1");
}

void test_net_all(void) {
    RUN(t_async_login_does_not_block);
    RUN(t_deferred_pipelined_order);
    RUN(t_deferred_client_gone);
    RUN(t_deferred_durable_barrier);
    RUN(t_listen_ipv4);
    RUN(t_listen_ipv6);
}
