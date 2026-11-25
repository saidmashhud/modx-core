#ifndef MODX_WS_H
#define MODX_WS_H

/**
 * @file ws.h
 * @brief WebSocket (RFC 6455) next to SSE on the same server and event loop.
 *
 * A handler accepts the upgrade with mx_ws_accept(); the connection then
 * carries frames. Complete messages (text or binary, fragmented or not) go
 * to http_server.on_ws_message; pings are answered, the closing handshake
 * is handled, the server pings idle sockets (ws_ping_ms) and drops ones that
 * stop answering. Protocol violations close with 1002, invalid UTF-8 text
 * with 1007, oversized messages with 1009. No extensions (no
 * permessage-deflate), no subprotocol negotiation beyond echoing one the
 * application picks.
 *
 * All functions run on the server's loop thread. Sends go through
 * http_conn_send(), so they respect the durability barrier.
 */

#include <stddef.h>
#include <stdint.h>
#include "modx/http.h"

/** Frame opcodes. */
enum { MX_WS_CONT = 0x0, /**< Continuation frame. */ MX_WS_TEXT = 0x1, /**< Text (UTF-8) message. */ MX_WS_BINARY = 0x2, /**< Binary message. */ MX_WS_CLOSE = 0x8, /**< Close. */ MX_WS_PING = 0x9, /**< Ping. */ MX_WS_PONG = 0xA /**< Pong. */ };

#define MX_WS_MAX_MESSAGE (1024 * 1024) /**< Default per-message limit (http_server.ws_max_message). */
#define MX_WS_PING_MS 30000             /**< Default ping interval for idle sockets; also the time allowed for the answer. */

/** Validate an upgrade request and turn resp into the 101 answer. app (may
 * be NULL) is bound to the connection like for SSE. subprotocol: NULL, or
 * the value to answer in Sec-WebSocket-Protocol. Returns 0, or an HTTP
 * status for a refused handshake (400 malformed or not a GET upgrade,
 * 426 wrong version), in which case resp is untouched except for a
 * "Sec-WebSocket-Version: 13" header on 426; the caller sets the status. */
int mx_ws_accept(http_conn *c, const http_request *req, http_response *resp, void *app, const char *subprotocol);
/** 1 when the request asks for a WebSocket upgrade (Upgrade: websocket and Connection: upgrade). */
int mx_ws_is_upgrade(const http_request *req);
/** Sec-WebSocket-Accept for a Sec-WebSocket-Key (out: 29 bytes incl. NUL). */
void mx_ws_accept_key(const char *key, char out[29]);

/** Queue one unfragmented frame (server frames are never masked). No-op unless
 * @p c is an open WebSocket that is not closing. */
void mx_ws_send(http_conn *c, int opcode, const void *data, size_t len);
/** mx_ws_send() of a NUL-terminated text message. */
void mx_ws_send_text(http_conn *c, const char *text);
/** Start the closing handshake (code 0 = no status; reason truncated to 123 bytes);
 * the connection closes once the frame is written. */
void mx_ws_close(http_conn *c, int code, const char *reason);
/** Send to every open WebSocket keep() accepts (NULL = all). Returns sockets told. */
size_t mx_ws_broadcast(http_server *srv, int (*keep)(void *ctx, http_conn *c), void *ctx, int opcode,
                       const void *data, size_t len);

/** Frame header parser (exposed for tests and fuzzing). */
typedef struct {
    int fin, rsv, opcode, masked; /**< FIN bit, RSV bits, opcode, MASK bit. */
    unsigned char mask[4]; /**< Masking key (when @c masked). */
    uint64_t len;      /**< Payload length. */
    size_t header;     /**< Header bytes (incl. mask). */
} mx_ws_frame;
/** Parse the frame at buf (payload left masked). >0: bytes of the complete frame;
 * 0: need more; -1002: protocol error (RSV set, unknown opcode, bad control
 * frame, non-minimal length); -1009: payload above max_payload. */
long mx_ws_parse_frame(const unsigned char *buf, size_t len, mx_ws_frame *f, size_t max_payload);
/** 1 when s[0..n) is valid UTF-8 (no overlongs, surrogates or > U+10FFFF). */
int mx_utf8_valid(const unsigned char *s, size_t n);

/** Called by the server loop for input on a WebSocket connection. */
void mx_ws_on_input(http_server *srv, http_conn *c);
/** Called by the server loop: pings / pong deadlines. */
void mx_ws_tick(http_server *srv, http_conn *c, int64_t now);

#endif
