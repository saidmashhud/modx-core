#ifndef MODX_TIMER_H
#define MODX_TIMER_H

/* Periodic jobs for the event loop: register callbacks with an interval and
 * call mx_timers_run from the server tick. Times are monotonic ms. */

#include <stddef.h>
#include <stdint.h>

typedef void (*mx_timer_fn)(void *ctx, int64_t now);

typedef struct {
    mx_timer_fn fn;
    void *ctx;
    int64_t every_ms;
    int64_t next_at;
} mx_timer;

typedef struct {
    mx_timer *t;
    size_t n, cap;
} mx_timers;

void mx_timers_init(mx_timers *ts);
void mx_timers_free(mx_timers *ts);
/* first_at: monotonic time of the first run (0 = now + every_ms). Returns the timer index. */
size_t mx_timers_add(mx_timers *ts, int64_t every_ms, int64_t first_at, mx_timer_fn fn, void *ctx);
/* Runs every due timer once (a late timer does not run twice). Returns how many ran. */
size_t mx_timers_run(mx_timers *ts, int64_t now);
/* Milliseconds until the next timer is due (>= 0), or -1 without timers. */
int64_t mx_timers_next_in(const mx_timers *ts, int64_t now);

#endif
