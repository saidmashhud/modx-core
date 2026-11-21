#include "modx/ws.h"
#include "modx/crypto.h"
#include "modx/util.h"

#include <string.h>
#include <strings.h>

static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void mx_ws_accept_key(const char *key, char out[29]) {
    sbuf b;
    sbuf_init(&b);
    sbuf_puts(&b, key);
    sbuf_puts(&b, WS_GUID);
    unsigned char h[SHA1_LEN];
    sha1(b.data, b.len, h);
    sbuf_free(&b);
    base64_encode(h, sizeof h, out);
}

/* Comma-separated header value contains token (case-insensitive). */
static int has_token(const char *v, const char *tok) {
    if (!v) return 0;
    size_t tl = strlen(tok);
    const char *p = v;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        const char *s = p;
        while (*p && *p != ',') p++;
        const char *e = p;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if ((size_t)(e - s) == tl && strncasecmp(s, tok, tl) == 0) return 1;
    }
    return 0;
}

int mx_ws_is_upgrade(const http_request *req) {
    return has_token(http_req_header(req, "upgrade"), "websocket") &&
           has_token(http_req_header(req, "connection"), "upgrade");
}

int mx_ws_accept(http_conn *c, const http_request *req, http_response *resp, void *app, const char *subprotocol) {
    if (strcmp(req->method, "GET") != 0 || !mx_ws_is_upgrade(req)) return 400;
    const char *ver = http_req_header(req, "sec-websocket-version");
    if (!ver || strcmp(ver, "13") != 0) {
        sbuf_puts(&resp->headers, "Sec-WebSocket-Version: 13\r\n");
        return 426;
    }
    const char *key = http_req_header(req, "sec-websocket-key");
    unsigned char raw[32];
    if (!key || strlen(key) != 24 || base64_decode(key, raw, sizeof raw) != 16) return 400;
    char accept[29];
    mx_ws_accept_key(key, accept);
    resp->status = 101;
    resp->ws = 1;
    resp->content_type = NULL;
    sbuf_printf(&resp->headers, "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n", accept);
    if (subprotocol) sbuf_printf(&resp->headers, "Sec-WebSocket-Protocol: %s\r\n", subprotocol);
    if (app) c->app = app;
    return 0;
}

static void frame_header(sbuf *out, int fin_opcode, size_t len) {
    unsigned char h[10];
    size_t n = 0;
    h[n++] = (unsigned char)fin_opcode;
    if (len < 126) {
        h[n++] = (unsigned char)len;
    } else if (len <= 0xFFFF) {
        h[n++] = 126;
        h[n++] = (unsigned char)(len >> 8);
        h[n++] = (unsigned char)len;
    } else {
        h[n++] = 127;
        for (int i = 7; i >= 0; i--) h[n++] = (unsigned char)((uint64_t)len >> (8 * i));
    }
    sbuf_put(out, h, n);
}

void mx_ws_send(http_conn *c, int opcode, const void *data, size_t len) {
    if (!c->is_ws || c->dead || c->ws_closing) return;
    sbuf f;
    sbuf_init(&f);
    frame_header(&f, 0x80 | (opcode & 0x0F), len);
    if (len) sbuf_put(&f, data, len);
    http_conn_send(c, f.data, f.len);
    sbuf_free(&f);
}

void mx_ws_send_text(http_conn *c, const char *text) { mx_ws_send(c, MX_WS_TEXT, text, strlen(text)); }

void mx_ws_close(http_conn *c, int code, const char *reason) {
    if (!c->is_ws || c->dead || c->ws_closing) return;
    unsigned char p[125];
    size_t n = 0;
    if (code) {
        p[n++] = (unsigned char)(code >> 8);
        p[n++] = (unsigned char)code;
        size_t rl = reason ? strlen(reason) : 0;
        if (rl > sizeof p - 2) rl = sizeof p - 2;
        if (rl) memcpy(p + n, reason, rl);
        n += rl;
    }
    mx_ws_send(c, MX_WS_CLOSE, p, n);
    c->ws_closing = 1;
    c->close_after_write = 1;
}

size_t mx_ws_broadcast(http_server *srv, int (*keep)(void *ctx, http_conn *c), void *ctx, int opcode,
                       const void *data, size_t len) {
    sbuf f;
    sbuf_init(&f);
    frame_header(&f, 0x80 | (opcode & 0x0F), len);
    if (len) sbuf_put(&f, data, len);
    size_t n = 0;
    for (size_t i = 0; i < srv->nconns; i++) {
        http_conn *c = srv->conns[i];
        if (!c->is_ws || c->dead || c->ws_closing) continue;
        if (keep && !keep(ctx, c)) continue;
        http_conn_send(c, f.data, f.len);
        n++;
    }
    sbuf_free(&f);
    return n;
}

long mx_ws_parse_frame(const unsigned char *b, size_t len, mx_ws_frame *f, size_t max_payload) {
    memset(f, 0, sizeof *f);
    if (len < 2) return 0;
    f->fin = (b[0] >> 7) & 1;
    f->rsv = (b[0] >> 4) & 7;
    f->opcode = b[0] & 0x0F;
    f->masked = (b[1] >> 7) & 1;
    uint64_t pl = b[1] & 0x7F;
    size_t h = 2;
    if (f->rsv) return -1002; /* no extensions negotiated */
    int control = f->opcode >= 0x8;
    if ((f->opcode > MX_WS_BINARY && f->opcode < MX_WS_CLOSE) || f->opcode > MX_WS_PONG) return -1002;
    if (control && (!f->fin || pl > 125)) return -1002;
    if (pl == 126) {
        if (len < 4) return 0;
        pl = (uint64_t)b[2] << 8 | b[3];
        if (pl < 126) return -1002; /* not minimal */
        h = 4;
    } else if (pl == 127) {
        if (len < 10) return 0;
        pl = 0;
        for (int i = 0; i < 8; i++) pl = pl << 8 | b[2 + i];
        if (pl >> 63 || pl <= 0xFFFF) return -1002;
        h = 10;
    }
    if (pl > max_payload) return -1009;
    if (f->masked) {
        if (len < h + 4) return 0;
        memcpy(f->mask, b + h, 4);
        h += 4;
    }
    f->len = pl;
    f->header = h;
    if (len < h + pl) return 0;
    return (long)(h + pl);
}

int mx_utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t k;
        uint32_t cp, min;
        if ((c & 0xE0) == 0xC0) { k = 1; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { k = 2; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { k = 3; cp = c & 0x07; min = 0x10000; }
        else return 0;
        if (i + k >= n) return 0; /* truncated sequence */
        for (size_t j = 1; j <= k; j++) {
            if ((s[i + j] & 0xC0) != 0x80) return 0;
            cp = cp << 6 | (s[i + j] & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += k + 1;
    }
    return 1;
}

/* Close for a protocol problem: send the close frame and stop reading. */
static void fail(http_conn *c, int code, const char *why) {
    mx_ws_close(c, code, why);
    c->in.len = 0;
}

void mx_ws_on_input(http_server *srv, http_conn *c) {
    size_t max = srv->ws_max_message ? srv->ws_max_message : MX_WS_MAX_MESSAGE;
    size_t off = 0;
    while (!c->dead && !c->ws_closing && off < c->in.len) {
        mx_ws_frame f;
        long n = mx_ws_parse_frame((const unsigned char *)c->in.data + off, c->in.len - off, &f, max);
        if (n == 0) break;
        if (n < 0) { fail(c, (int)-n, n == -1009 ? "message too big" : "protocol error"); return; }
        if (!f.masked) { fail(c, 1002, "client frames must be masked"); return; }
        unsigned char *pl = (unsigned char *)c->in.data + off + f.header;
        for (uint64_t i = 0; i < f.len; i++) pl[i] ^= f.mask[i & 3];
        off += (size_t)n;
        c->last_active = mono_ms();
        c->ws_ping_due = 0;
        switch (f.opcode) {
        case MX_WS_PING:
            mx_ws_send(c, MX_WS_PONG, pl, (size_t)f.len);
            break;
        case MX_WS_PONG:
            break;
        case MX_WS_CLOSE: {
            int code = 1000;
            if (f.len == 1) { fail(c, 1002, "bad close frame"); return; }
            if (f.len >= 2) {
                code = pl[0] << 8 | pl[1];
                int valid = (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1011) || (code >= 3000 && code <= 4999);
                if (!valid || !mx_utf8_valid(pl + 2, (size_t)f.len - 2)) { fail(c, 1002, "bad close frame"); return; }
            }
            mx_ws_close(c, f.len ? code : 0, NULL); /* echo, then close the TCP connection */
            c->in.len = 0;
            return;
        }
        case MX_WS_TEXT:
        case MX_WS_BINARY:
            if (c->ws_msg_op) { fail(c, 1002, "expected a continuation frame"); return; }
            c->ws_msg_op = f.opcode;
            c->ws_msg.len = 0;
            /* fall through */
        case MX_WS_CONT:
            if (!c->ws_msg_op) { fail(c, 1002, "continuation without a message"); return; }
            if (c->ws_msg.len + f.len > max) { fail(c, 1009, "message too big"); return; }
            sbuf_put(&c->ws_msg, pl, (size_t)f.len);
            if (f.fin) {
                int op = c->ws_msg_op;
                c->ws_msg_op = 0;
                if (op == MX_WS_TEXT && !mx_utf8_valid((const unsigned char *)c->ws_msg.data, c->ws_msg.len)) {
                    fail(c, 1007, "invalid UTF-8");
                    return;
                }
                if (!c->ws_msg.data) sbuf_reserve(&c->ws_msg, 0);
                srv->ws_messages++;
                if (srv->on_ws_message) srv->on_ws_message(srv, c, op, c->ws_msg.data, c->ws_msg.len, srv->ctx);
                c->ws_msg.len = 0;
                if (c->ws_msg.cap > 65536) sbuf_shrink(&c->ws_msg);
            }
            break;
        }
    }
    if (off) sbuf_consume(&c->in, off);
}

void mx_ws_tick(http_server *srv, http_conn *c, int64_t now) {
    int64_t every = srv->ws_ping_ms ? srv->ws_ping_ms : MX_WS_PING_MS;
    if (c->ws_closing) return;
    if (c->ws_ping_due && now > c->ws_ping_due) { c->dead = 1; srv->timeouts++; return; } /* no answer */
    if (!c->ws_ping_due && now - c->last_active > every) {
        mx_ws_send(c, MX_WS_PING, "", 0);
        c->ws_ping_due = now + every; /* any frame from the peer clears it */
    }
}
