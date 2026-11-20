/* Periodic timers and SSE event framing. */
#include <stdlib.h>

#include "modx/sse.h"
#include "modx/timer.h"
#include "test.h"

static int fired[3];
static void bump(void *ctx, int64_t now) { (void)now; fired[(int)(intptr_t)ctx]++; }

static void t_timers(void) {
    mx_timers ts;
    mx_timers_init(&ts);
    CHECK(mx_timers_next_in(&ts, 0) == -1);
    mx_timers_add(&ts, 100, 1000, bump, (void *)(intptr_t)0);
    mx_timers_add(&ts, 250, 1000, bump, (void *)(intptr_t)1);
    CHECK(mx_timers_run(&ts, 999) == 0);
    CHECK(mx_timers_next_in(&ts, 990) == 10);
    CHECK(mx_timers_run(&ts, 1000) == 2);
    CHECK(mx_timers_run(&ts, 1100) == 1 && fired[0] == 2 && fired[1] == 1);
    /* far behind: one run, then back on the interval */
    CHECK(mx_timers_run(&ts, 5000) == 2 && fired[0] == 3);
    CHECK(mx_timers_run(&ts, 5050) == 0);
    CHECK(mx_timers_run(&ts, 5100) == 1);
    mx_timers_free(&ts);
}

static void t_sse_format(void) {
    sbuf b;
    sbuf_init(&b);
    mx_sse_format(&b, NULL, 7, "{\"a\":1}");
    CHECK_STR(b.data, "id: 7\ndata: {\"a\":1}\n\n");
    b.len = 0;
    mx_sse_format(&b, "reset", -1, "line1\nline2");
    CHECK_STR(b.data, "event: reset\ndata: line1\ndata: line2\n\n");
    sbuf_free(&b);
}

void test_timer_all(void) {
    RUN(t_timers);
    RUN(t_sse_format);
}
