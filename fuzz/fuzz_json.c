/* Fuzz target: JSON parser and writer (parse -> dump -> parse round trip). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "modx/json.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    char err[128];
    json *v = json_parse((const char *)data, size, err, sizeof err);
    if (!v) return 0;
    char *s = json_dump(v);
    json *w = json_parse(s, strlen(s), err, sizeof err);
    if (!w || !json_equal(v, w)) abort(); /* what we write we must read back identically */
    json *c = json_clone(v);
    if (!json_equal(c, v)) abort();
    json_free(c);
    json_free(w);
    free(s);
    json_free(v);
    return 0;
}
