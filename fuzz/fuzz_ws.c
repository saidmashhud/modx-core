/* Fuzz target: WebSocket frame parser and UTF-8 validator over a stream of frames. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "modx/ws.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    size_t off = 0;
    for (int k = 0; k < 64 && off < size; k++) {
        mx_ws_frame f;
        long n = mx_ws_parse_frame(data + off, size - off, &f, 1 << 16);
        if (n <= 0) break;
        if ((size_t)n > size - off || f.header + f.len != (uint64_t)n || f.rsv || (f.opcode >= 8 && (!f.fin || f.len > 125)))
            abort();
        (void)mx_utf8_valid(data + off + f.header, (size_t)f.len);
        off += (size_t)n;
    }
    (void)mx_utf8_valid(data, size);
    return 0;
}
