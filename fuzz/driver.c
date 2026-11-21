/* Stand-alone fuzz driver for toolchains without libFuzzer (Apple clang):
 * feeds LLVMFuzzerTestOneInput with seeds and random mutations of them.
 *   ./fuzz_x [iterations] [seed]
 * Build with -fsanitize=address,undefined so memory errors abort. The same
 * targets link against libFuzzer with clang -fsanitize=fuzzer. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
extern const char *const FUZZ_SEEDS[];
extern const char *const FUZZ_DICT[];

static uint64_t rs;
static uint64_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static size_t mutate(uint8_t *buf, size_t len, size_t cap) {
    int rounds = 1 + (int)(rnd() % 6);
    for (int r = 0; r < rounds; r++) {
        switch (rnd() % 8) {
        case 0: if (len) buf[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8)); break;           /* bit flip */
        case 1: if (len) buf[rnd() % len] = (uint8_t)rnd(); break;                          /* random byte */
        case 2: if (len < cap) { size_t i = rnd() % (len + 1); memmove(buf + i + 1, buf + i, len - i); buf[i] = (uint8_t)rnd(); len++; } break;
        case 3: if (len) { size_t i = rnd() % len; memmove(buf + i, buf + i + 1, len - i - 1); len--; } break;
        case 4: if (len) len = rnd() % len; break;                                          /* truncate */
        case 5: {                                                                          /* dictionary token */
            size_t nd = 0;
            while (FUZZ_DICT[nd]) nd++;
            if (!nd) break;
            const char *t = FUZZ_DICT[rnd() % nd];
            size_t tl = strlen(t), i = rnd() % (len + 1);
            if (len + tl > cap) break;
            memmove(buf + i + tl, buf + i, len - i);
            memcpy(buf + i, t, tl);
            len += tl;
            break;
        }
        case 6: if (len > 1) { size_t a = rnd() % len, n = 1 + rnd() % (len - a); if (len + n <= cap) { memmove(buf + a + n, buf + a, len - a); len += n; } } break; /* duplicate */
        default: if (len) { size_t i = rnd() % len; buf[i] = (uint8_t)("0123456789-:/\r\n\"{}[],%"[rnd() % 23]); } break;
        }
    }
    return len;
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 100000;
    rs = argc > 2 ? strtoull(argv[2], NULL, 10) : 0x2545F4914F6CDD1DULL;
    if (!rs) rs = 1;
    size_t ns = 0;
    while (FUZZ_SEEDS[ns]) ns++;
    const size_t cap = 1 << 16;
    uint8_t *buf = malloc(cap);
    for (size_t i = 0; i < ns; i++) LLVMFuzzerTestOneInput((const uint8_t *)FUZZ_SEEDS[i], strlen(FUZZ_SEEDS[i]));
    for (long it = 0; it < iters; it++) {
        const char *s = FUZZ_SEEDS[rnd() % ns];
        size_t len = strlen(s);
        if (len > cap) len = cap;
        memcpy(buf, s, len);
        len = mutate(buf, len, cap);
        LLVMFuzzerTestOneInput(buf, len);
    }
    free(buf);
    fprintf(stderr, "%s: %ld iterations, no crash\n", argv[0], iters);
    return 0;
}
