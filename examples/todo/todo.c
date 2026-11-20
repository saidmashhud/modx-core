/* examples/todo — a minimal server on modx-core.
 *
 *   GET    /api/health
 *   GET    /api/todos?list=<id>&limit=<n>   rows of one list (or all), newest first
 *   GET    /api/todos/<id>
 *   POST   /api/todos        {"title","list"?}          -> 201 row
 *   PUT    /api/todos/<id>   {"title"?,"done"?}         -> row
 *   DELETE /api/todos/<id>                              -> 204
 *   GET    /api/changes?since=<seq>                     -> [change, ...] or 410
 *   GET    /api/events                                  -> SSE (Last-Event-ID catch-up)
 *   GET    /api/stats                                   -> counters (bench)
 *   POST   /api/compact[?bg=1]                          -> compact now / in the background (TODO_ADMIN=1)
 *   POST   /api/fault                                   -> fail the next commit (TODO_ADMIN=1, tests)
 *   anything else: static files from TODO_STATIC (default examples/todo/public)
 *
 * Environment: TODO_HOST (127.0.0.1), TODO_PORT (8080, 0 = any), TODO_DATA
 * (./todo-data), TODO_STATIC, TODO_SEED=<n> (seed n rows into an empty store),
 * TODO_COMPACT_BYTES, TODO_MAX_CONNS, TODO_ADMIN=1. */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "modx/http.h"
#include "modx/json.h"
#include "modx/sse.h"
#include "modx/static.h"
#include "modx/store.h"
#include "modx/util.h"

enum { C_TODOS, C_NCOLL };
static const mx_coll_def COLLS[C_NCOLL] = { { "todos", 0 } };

typedef struct {
    mx_store *st;
    mx_sse sse;
    const char *static_dir;
    int admin;
} app;

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static long env_long(const char *name, long fallback) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    char *e;
    long n = strtol(v, &e, 10);
    return *e ? fallback : n;
}

static void reply_json(http_response *resp, int status, const char *body) {
    resp->status = status;
    resp->content_type = "application/json; charset=utf-8";
    sbuf_puts(&resp->body, body);
}

static void reply_err(http_response *resp, int status, const char *code) {
    resp->status = status;
    resp->content_type = "application/json; charset=utf-8";
    sbuf_printf(&resp->body, "{\"error\":{\"code\":\"%s\"}}", code);
}

static int cmp_updated_desc(const void *a, const void *b) {
    double x = json_get_num(*(const json *const *)a, "updated_at", 0);
    double y = json_get_num(*(const json *const *)b, "updated_at", 0);
    return x < y ? 1 : x > y ? -1 : 0;
}

static void list_todos(app *a, const http_request *req, http_response *resp) {
    const char *list = http_req_query(req, "list");
    const char *lim = http_req_query(req, "limit");
    long limit = lim ? strtol(lim, NULL, 10) : 100;
    if (limit <= 0 || limit > 1000) limit = 100;
    size_t n;
    const json **all = mx_store_all(a->st, C_TODOS, &n);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *l = json_get_str(all[i], "list");
        if (!list || (l && strcmp(l, list) == 0)) all[k++] = all[i];
    }
    qsort(all, k, sizeof *all, cmp_updated_desc);
    if (k > (size_t)limit) k = (size_t)limit;
    resp->content_type = "application/json; charset=utf-8";
    sbuf_putc(&resp->body, '[');
    for (size_t i = 0; i < k; i++) {
        if (i) sbuf_putc(&resp->body, ',');
        json_write(&resp->body, all[i]);
    }
    sbuf_putc(&resp->body, ']');
    free(all);
}

static json *parse_body(const http_request *req) {
    if (!req->body_len) return NULL;
    json *b = json_parse(req->body, req->body_len, NULL, 0);
    if (b && b->type != JSON_OBJECT) { json_free(b); return NULL; }
    return b;
}

static void write_row(app *a, http_response *resp, int status, const char *id) {
    resp->status = status;
    resp->content_type = "application/json; charset=utf-8";
    json_write(&resp->body, mx_store_get(a->st, C_TODOS, id));
}

static void create_todo(app *a, const http_request *req, http_response *resp) {
    json *b = parse_body(req);
    const char *title = json_get_str(b, "title");
    if (!title || !*title) { json_free(b); reply_err(resp, 400, "title_required"); return; }
    char *id = gen_id("td");
    json *row = json_obj();
    json_set(row, "id", json_str(id));
    json_set(row, "title", json_str(title));
    json_set(row, "list", json_str(json_get_str(b, "list") ? json_get_str(b, "list") : "inbox"));
    json_set(row, "done", json_bool(0));
    json_set(row, "updated_at", json_num((double)now_ms()));
    json_free(b);
    if (mx_store_put(a->st, C_TODOS, row, http_req_header(req, "x-client-id")) != 0)
        reply_err(resp, 500, "storage");
    else write_row(a, resp, 201, id);
    free(id);
}

static void update_todo(app *a, const http_request *req, http_response *resp, const char *id) {
    const json *cur = mx_store_get(a->st, C_TODOS, id);
    if (!cur) { reply_err(resp, 404, "not_found"); return; }
    json *b = parse_body(req);
    if (!b) { reply_err(resp, 400, "bad_json"); return; }
    json *row = json_clone(cur);
    const char *title = json_get_str(b, "title");
    if (title) json_set(row, "title", json_str(title));
    const json *done = json_get(b, "done");
    if (done && done->type == JSON_BOOL) json_set(row, "done", json_bool(done->boolean));
    json_set(row, "updated_at", json_num((double)now_ms()));
    json_free(b);
    if (mx_store_put(a->st, C_TODOS, row, http_req_header(req, "x-client-id")) != 0)
        reply_err(resp, 500, "storage");
    else write_row(a, resp, 200, id);
}

static void append_change(void *ctx, const mx_change *ch) {
    sbuf *out = ctx;
    if (out->len > 1) sbuf_putc(out, ',');
    sbuf_puts(out, ch->json);
}

static void replay_change(void *ctx, const mx_change *ch) { mx_sse_format(ctx, NULL, ch->seq, ch->json); }

static void open_events(app *a, http_conn *c, const http_request *req, http_response *resp) {
    mx_sse_open(c, resp, NULL);
    const char *last = http_req_header(req, "last-event-id");
    if (last && *last) {
        long long since = strtoll(last, NULL, 10);
        if (mx_store_for_each_since(a->st, since, NULL, NULL, replay_change, &resp->body) != 0)
            sbuf_printf(&resp->body, "event: reset\ndata: {\"seq\":%lld}\n\n", (long long)a->st->seq);
    }
}

static long rss_kb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / 1024; /* bytes on macOS */
#else
    return ru.ru_maxrss;        /* KiB on Linux */
#endif
}

static void handle(http_server *srv, http_conn *c, const http_request *req, http_response *resp, void *ctx) {
    app *a = ctx;
    const char *m = req->method, *p = req->path;
    int get = strcmp(m, "GET") == 0;
    if (strncmp(p, "/api/", 5) != 0) {
        if (get || strcmp(m, "HEAD") == 0) static_serve(a->static_dir, req, resp);
        else reply_err(resp, 404, "not_found");
        return;
    }
    if (get && strcmp(p, "/api/health") == 0) { reply_json(resp, 200, "{\"ok\":true}"); return; }
    if (get && strcmp(p, "/api/events") == 0) { open_events(a, c, req, resp); return; }
    if (get && strcmp(p, "/api/stats") == 0) {
        resp->content_type = "application/json; charset=utf-8";
        sbuf_printf(&resp->body,
                    "{\"seq\":%lld,\"rows\":%zu,\"journal_bytes\":%lld,\"open_ms\":%lld,\"last_compact_ms\":%lld,"
                    "\"compactions\":%lld,\"compacting\":%s,\"fsyncs\":%lld,\"max_batch\":%lld,\"conns\":%zu,\"peak_conns\":%zu,\"streams\":%zu,\"max_rss_kb\":%ld}",
                    (long long)a->st->seq, mx_store_count(a->st, C_TODOS), (long long)a->st->journal_size,
                    (long long)a->st->open_ms, (long long)a->st->last_compact_ms, (long long)a->st->compactions,
                    a->st->compact_pid > 0 ? "true" : "false", (long long)a->st->fsyncs, (long long)a->st->max_batch,
                    srv->nconns, srv->nconns_peak, mx_sse_count(&a->sse), rss_kb());
        return;
    }
    if (strcmp(p, "/api/compact") == 0 && strcmp(m, "POST") == 0) {
        if (!a->admin) { reply_err(resp, 403, "forbidden"); return; }
        int64_t t0 = mono_ms();
        const char *bg = http_req_query(req, "bg");
        int rc = bg && strcmp(bg, "1") == 0 ? mx_store_compact_bg(a->st) : mx_store_compact(a->st);
        resp->content_type = "application/json; charset=utf-8";
        sbuf_printf(&resp->body, "{\"ok\":%s,\"blocked_ms\":%lld}", rc >= 0 ? "true" : "false",
                    (long long)(mono_ms() - t0));
        return;
    }
    if (strcmp(p, "/api/fault") == 0 && strcmp(m, "POST") == 0) {
        if (!a->admin) { reply_err(resp, 403, "forbidden"); return; }
        a->st->fault_syncs = 1; /* the next group commit fails */
        reply_json(resp, 200, "{\"ok\":true}");
        return;
    }
    if (get && strcmp(p, "/api/changes") == 0) {
        const char *s = http_req_query(req, "since");
        long long since = s ? strtoll(s, NULL, 10) : 0;
        sbuf out;
        sbuf_init(&out);
        sbuf_putc(&out, '[');
        if (mx_store_for_each_since(a->st, since, NULL, NULL, append_change, &out) != 0) {
            sbuf_free(&out);
            reply_err(resp, 410, "gone");
            return;
        }
        sbuf_putc(&out, ']');
        resp->content_type = "application/json; charset=utf-8";
        sbuf_put(&resp->body, out.data, out.len);
        sbuf_free(&out);
        return;
    }
    if (strcmp(p, "/api/todos") == 0) {
        if (get) list_todos(a, req, resp);
        else if (strcmp(m, "POST") == 0) create_todo(a, req, resp);
        else reply_err(resp, 405, "method_not_allowed");
        return;
    }
    if (strncmp(p, "/api/todos/", 11) == 0 && p[11] && !strchr(p + 11, '/')) {
        const char *id = p + 11;
        if (get) {
            if (mx_store_get(a->st, C_TODOS, id)) write_row(a, resp, 200, id);
            else reply_err(resp, 404, "not_found");
        } else if (strcmp(m, "PUT") == 0) {
            update_todo(a, req, resp, id);
        } else if (strcmp(m, "DELETE") == 0) {
            if (!mx_store_get(a->st, C_TODOS, id)) { reply_err(resp, 404, "not_found"); return; }
            if (mx_store_del(a->st, C_TODOS, id, NULL) != 0) reply_err(resp, 500, "storage");
            else resp->status = 204;
        } else {
            reply_err(resp, 405, "method_not_allowed");
        }
        return;
    }
    reply_err(resp, 404, "not_found");
}

/* Durable change -> every SSE stream. */
static void on_durable(void *ctx, const mx_change *ch) {
    app *a = ctx;
    mx_sse_broadcast(&a->sse, NULL, NULL, NULL, ch->seq, ch->json);
}

static void tick(http_server *srv, void *ctx) {
    (void)srv;
    app *a = ctx;
    mx_sse_tick(&a->sse);
    mx_store_tick(a->st);
}

static int seed(mx_store *st, long n) {
    static const char *lists[] = { "inbox", "work", "home", "errands", "ideas", "reading", "travel", "garden" };
    char id[32], title[64];
    int64_t t = now_ms();
    for (long i = 0; i < n; i++) {
        snprintf(id, sizeof id, "td_seed%07ld", i);
        snprintf(title, sizeof title, "Seeded task number %ld", i);
        json *row = json_obj();
        json_set(row, "id", json_str(id));
        json_set(row, "title", json_str(title));
        /* 1000 lists of ~n/1000 rows, plus the named ones */
        char list[32];
        if (i % 2) snprintf(list, sizeof list, "%s", lists[i % 8]);
        else snprintf(list, sizeof list, "list%03ld", (i / 2) % 1000);
        json_set(row, "list", json_str(list));
        json_set(row, "done", json_bool(i % 3 == 0));
        json_set(row, "updated_at", json_num((double)(t - n + i)));
        if (mx_store_put(st, C_TODOS, row, NULL) != 0) return -1;
    }
    return mx_store_sync(st);
}

int main(void) {
    const char *host = getenv("TODO_HOST") ? getenv("TODO_HOST") : "127.0.0.1";
    long port = env_long("TODO_PORT", 8080);
    const char *dir = getenv("TODO_DATA") ? getenv("TODO_DATA") : "./todo-data";
    app a;
    memset(&a, 0, sizeof a);
    a.static_dir = getenv("TODO_STATIC") ? getenv("TODO_STATIC") : "examples/todo/public";
    a.admin = env_long("TODO_ADMIN", 0) == 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 8192) {
        rl.rlim_cur = rl.rlim_max < 8192 ? rl.rlim_max : 8192;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    int64_t t0 = mono_ms();
    char err[256];
    a.st = mx_store_open(dir, COLLS, C_NCOLL, 10000, NULL, err, sizeof err);
    if (!a.st) { fprintf(stderr, "todo: %s\n", err); return 1; }
    long compact = env_long("TODO_COMPACT_BYTES", 0);
    if (compact > 0) a.st->compact_threshold = compact;
    long nseed = env_long("TODO_SEED", 0);
    if (nseed > 0 && mx_store_is_empty(a.st)) {
        if (seed(a.st, nseed) != 0 || mx_store_compact(a.st) != 0) { fprintf(stderr, "todo: seed failed\n"); return 1; }
        fprintf(stderr, "todo: seeded %ld rows\n", nseed);
    }

    http_server srv;
    memset(&srv, 0, sizeof srv);
    srv.listen_fd = -1;
    srv.max_conns = (size_t)env_long("TODO_MAX_CONNS", 4096);
    if (http_server_listen(&srv, host, (int)port) != 0) {
        fprintf(stderr, "todo: cannot listen on %s:%ld: %s\n", host, port, strerror(errno));
        return 1;
    }
    mx_sse_init(&a.sse, &srv, 15000);
    a.st->listener = on_durable;
    a.st->listener_ctx = &a;
    /* group commit: one fsync per loop iteration, responses after it */
    srv.durable_pending = mx_store_pending_cb;
    srv.durable_commit = mx_store_commit_cb;
    srv.durable_ctx = a.st;
    srv.handler = handle;
    srv.tick = tick;
    srv.ctx = &a;
    printf("todo listening on http://%s:%d (rows=%zu, seq=%lld, open_ms=%lld, startup_ms=%lld)\n", host, srv.port,
           mx_store_count(a.st, C_TODOS), (long long)a.st->seq, (long long)a.st->open_ms,
           (long long)(mono_ms() - t0));
    fflush(stdout);
    http_server_run(&srv, &g_stop);
    http_server_close(&srv);
    int rc = mx_store_sync(a.st) == 0 ? 0 : 1;
    mx_store_close(a.st);
    return rc;
}
