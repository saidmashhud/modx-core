#include "modx/sse.h"

#include <string.h>

void mx_sse_init(mx_sse *hub, http_server *srv, int64_t ping_ms) {
    hub->srv = srv;
    hub->ping_ms = ping_ms > 0 ? ping_ms : 15000;
    hub->last_ping = mono_ms();
}

void mx_sse_open(http_conn *c, http_response *resp, void *app) {
    resp->sse = 1;
    if (app) c->app = app;
    sbuf_puts(&resp->body, "retry: 2000\n\n");
}

void mx_sse_format(sbuf *out, const char *event, int64_t id, const char *data) {
    if (id >= 0) sbuf_printf(out, "id: %lld\n", (long long)id);
    if (event) sbuf_printf(out, "event: %s\n", event);
    const char *p = data ? data : "";
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        sbuf_puts(out, "data: ");
        sbuf_put(out, p, n);
        sbuf_putc(out, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    sbuf_putc(out, '\n');
}

void mx_sse_send(http_conn *c, const char *event, int64_t id, const char *data) {
    sbuf b;
    sbuf_init(&b);
    mx_sse_format(&b, event, id, data);
    http_conn_send(c, b.data, b.len);
    sbuf_free(&b);
}

size_t mx_sse_broadcast(mx_sse *hub, mx_sse_filter keep, void *ctx, const char *event, int64_t id, const char *data) {
    sbuf b;
    sbuf_init(&b);
    mx_sse_format(&b, event, id, data);
    size_t n = 0;
    for (size_t i = 0; i < hub->srv->nconns; i++) {
        http_conn *c = hub->srv->conns[i];
        if (!c->is_sse || c->dead) continue;
        if (keep && !keep(ctx, c)) continue;
        http_conn_send(c, b.data, b.len);
        n++;
    }
    sbuf_free(&b);
    return n;
}

void mx_sse_tick(mx_sse *hub) {
    int64_t now = mono_ms();
    if (now - hub->last_ping < hub->ping_ms) return;
    hub->last_ping = now;
    for (size_t i = 0; i < hub->srv->nconns; i++) {
        http_conn *c = hub->srv->conns[i];
        if (c->is_sse && !c->dead) http_conn_send(c, ": ping\n\n", 8);
    }
}

size_t mx_sse_count(const mx_sse *hub) {
    size_t n = 0;
    for (size_t i = 0; i < hub->srv->nconns; i++)
        if (hub->srv->conns[i]->is_sse && !hub->srv->conns[i]->dead) n++;
    return n;
}
