const char *const FUZZ_SEEDS[] = {
    "GET / HTTP/1.1\r\nHost: a\r\n\r\n",
    "GET /api/x%20y?a=1&b=two+words&c=%2F HTTP/1.1\r\nHost: a\r\nCookie: s=1\r\n\r\n",
    "POST /api/todos HTTP/1.1\r\nHost: a\r\nContent-Type: application/json\r\nContent-Length: 13\r\n\r\n{\"title\":\"x\"}",
    "POST /up HTTP/1.1\r\nHost: a\r\nContent-Length: 2000000\r\nExpect: 100-continue\r\n\r\n",
    "GET /a HTTP/1.1\r\nHost: a\r\n\r\nGET /b HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
    "GET /ws HTTP/1.1\r\nHost: a\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n",
    0,
};
const char *const FUZZ_DICT[] = { "\r\n", "\r\n\r\n", ": ", "Content-Length: ", "Transfer-Encoding: chunked", "%00", "%2",
                                  "HTTP/1.1", "HTTP/1.0", "?", "&", "=", "Expect: 100-continue", "99999999999999999999", 0 };
