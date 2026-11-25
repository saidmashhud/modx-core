/* A minimal modx-core server (docs/getting-started.md walks through it):
 *   GET  /api/notes    the notes, as a JSON array
 *   POST /api/notes    {"text": "..."} -> 201 with the stored row (sent once it is on disk)
 *   GET  /api/events   Server-Sent Events: every durable change
 *   POST /api/login    {"password": "..."} -> 200 / 401, verified on a worker thread
 * MINIMAL_HOST (default 127.0.0.1; "::" = IPv4 + IPv6), MINIMAL_PORT (8080),
 * MINIMAL_DATA (./minimal-data). */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modx/crypto.h"
#include "modx/http.h"
#include "modx/json.h"
#include "modx/pool.h"
#include "modx/sse.h"
#include "modx/store.h"

enum { C_NOTES, C_NCOLL };
static const mx_coll_def COLLS[C_NCOLL] = { { "notes", 0, NULL } };

typedef struct {
    mx_store *st;
    mx_sse sse;
    mx_pool *pool;
    char *password_hash; /* the one account of this example */
} app;

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void reply_error(http_response *resp, int status, const char *msg) {
    resp->status = status;
    resp->body.len = 0;
    sbuf_printf(&resp->body, "{\"error\":\"%s\"}", msg);
}

/* ---- login: the password check runs on the pool, the answer is deferred ---- */
typedef struct {
    http_deferred *d;
} login;

static void login_done(void *ctx, int ok, int needs_rehash) {
    login *l = ctx;
    (void)needs_rehash; /* a real server would store password_hash_async's new hash here */
    http_response *r = http_deferred_response(l->d);
    if (ok) sbuf_puts(&r->body, "{\"ok\":true}");
    else reply_error(r, 401, "invalid password");
    http_deferred_finish(l->d); /* sent now (or after the barrier if changes are pending) */
    free(l);
}

static void handle_login(app *a, http_conn *c, const http_request *req, http_response *resp) {
    json *body = json_parse(req->body, req->body_len, NULL, 0);
    const char *pw = json_get_str(body, "password");
    if (!pw) { json_free(body); reply_error(resp, 400, "password required"); return; }
    login *l = malloc(sizeof *l);
    l->d = http_defer(c, req, resp); /* resp is ignored from here on */
    if (password_verify_async(a->pool, pw, a->password_hash, login_done, l) != 0) {
        reply_error(http_deferred_response(l->d), 503, "busy"); /* pool queue full */
        http_deferred_finish(l->d);
        free(l);
    }
    json_free(body); /* the pool job has its own copy of the password */
}

/* ---- routes ---- */
static void handle(http_server *srv, http_conn *c, const http_request *req, http_response *resp, void *ctx) {
    (void)srv;
    app *a = ctx;
    int get = strcmp(req->method, "GET") == 0, post = strcmp(req->method, "POST") == 0;
    if (get && strcmp(req->path, "/api/events") == 0) {
        mx_sse_open(c, resp, NULL);
        return;
    }
    if (get && strcmp(req->path, "/api/notes") == 0) {
        size_t n;
        const json **rows = mx_store_all(a->st, C_NOTES, &n);
        sbuf_putc(&resp->body, '[');
        for (size_t i = 0; i < n; i++) {
            if (i) sbuf_putc(&resp->body, ',');
            json_write(&resp->body, rows[i]);
        }
        sbuf_putc(&resp->body, ']');
        free(rows);
        return;
    }
    if (post && strcmp(req->path, "/api/notes") == 0) {
        json *body = json_parse(req->body, req->body_len, NULL, 0);
        const char *text = json_get_str(body, "text");
        if (!text) { json_free(body); reply_error(resp, 400, "text required"); return; }
        json *row = json_obj();
        char *id = gen_id("note");
        json_set(row, "id", json_str(id));
        json_set(row, "text", json_str(text));
        json_write(&resp->body, row);
        resp->status = 201;
        /* queued for the journal; the response is held until the barrier made it durable */
        if (mx_store_put(a->st, C_NOTES, row, http_req_header(req, "x-client-id")) != 0)
            reply_error(resp, 503, "storage is read-only");
        free(id);
        json_free(body);
        return;
    }
    if (post && strcmp(req->path, "/api/login") == 0) { handle_login(a, c, req, resp); return; }
    reply_error(resp, 404, "not found");
}

/* after fsync, in seq order: fan the change out */
static void on_durable(void *ctx, const mx_change *ch) {
    app *a = ctx;
    mx_sse_broadcast(&a->sse, NULL, NULL, NULL, ch->seq, ch->json);
}

static void tick(http_server *srv, void *ctx) {
    (void)srv;
    app *a = ctx;
    mx_sse_tick(&a->sse);  /* pings */
    mx_store_tick(a->st);  /* reaps a background compaction */
}

int main(void) {
    const char *host = getenv("MINIMAL_HOST") ? getenv("MINIMAL_HOST") : "127.0.0.1";
    int port = getenv("MINIMAL_PORT") ? atoi(getenv("MINIMAL_PORT")) : 8080;
    const char *dir = getenv("MINIMAL_DATA") ? getenv("MINIMAL_DATA") : "./minimal-data";
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    app a;
    memset(&a, 0, sizeof a);
    char err[512];
    a.st = mx_store_open(dir, COLLS, C_NCOLL, 1000, NULL, err, sizeof err);
    if (!a.st) { fprintf(stderr, "minimal: %s\n", err); return 1; }
    a.pool = mx_pool_new(2, 0);
    a.password_hash = password_hash("demo-password"); /* once, at startup: about 0.5 s */
    if (!a.pool || !a.password_hash) { fprintf(stderr, "minimal: no worker pool / randomness\n"); return 1; }

    http_server srv;
    memset(&srv, 0, sizeof srv);
    srv.listen_fd = -1;
    if (http_server_listen(&srv, host, port) != 0) {
        fprintf(stderr, "minimal: cannot listen on %s:%d: %s\n", host, port, strerror(errno));
        return 1;
    }
    mx_sse_init(&a.sse, &srv, 15000);
    mx_pool_attach(a.pool, &srv); /* completions run on this thread */
    a.st->listener = on_durable;
    a.st->listener_ctx = &a;
    srv.durable_pending = mx_store_pending_cb; /* group commit: one write + fsync per loop iteration */
    srv.durable_commit = mx_store_commit_cb;
    srv.durable_ctx = a.st;
    srv.handler = handle;
    srv.tick = tick;
    srv.ctx = &a;
    printf("minimal listening on port %d\n", srv.port);
    fflush(stdout);
    http_server_run(&srv, &g_stop); /* until SIGINT / SIGTERM, then drains */
    http_server_close(&srv);
    mx_pool_free(a.pool);
    int rc = mx_store_sync(a.st) == 0 ? 0 : 1;
    mx_store_close(a.st);
    free(a.password_hash);
    return rc;
}
