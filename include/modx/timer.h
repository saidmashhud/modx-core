#ifndef MODX_TIMER_H
#define MODX_TIMER_H

/**
 * @file timer.h
 * @brief Periodic jobs for the event loop: register callbacks with an interval and
 * call mx_timers_run from the server tick. Times are monotonic ms (mono_ms()).
 */

#include <stddef.h>
#include <stdint.h>

/** Timer callback; @p now is the time passed to mx_timers_run(). */
typedef void (*mx_timer_fn)(void *ctx, int64_t now);

/** One periodic job. */
typedef struct {
    mx_timer_fn fn;   /**< Callback. */
    void *ctx;        /**< Passed to @c fn. */
    int64_t every_ms; /**< Interval (>= 1). */
    int64_t next_at;  /**< Monotonic ms of the next run. */
} mx_timer;

/** A set of timers (growable array). */
typedef struct {
    mx_timer *t;   /**< Timers. */
    size_t n, cap; /**< Count and capacity of @c t. */
} mx_timers;

/** Initialize an empty set. */
void mx_timers_init(mx_timers *ts);
/** Free the set (leaves it empty and reusable). */
void mx_timers_free(mx_timers *ts);
/** Add a timer. every_ms < 1 is treated as 1. first_at: monotonic time of the
 * first run (0 = now + every_ms). Returns the timer index. Timers cannot be removed. */
size_t mx_timers_add(mx_timers *ts, int64_t every_ms, int64_t first_at, mx_timer_fn fn, void *ctx);
/** Runs every due timer once (a late timer does not run twice; missed runs are skipped). Returns how many ran. */
size_t mx_timers_run(mx_timers *ts, int64_t now);
/** Milliseconds until the next timer is due (>= 0), or -1 without timers. */
int64_t mx_timers_next_in(const mx_timers *ts, int64_t now);

#endif
