#include "modx/timer.h"
#include "modx/util.h"

#include <stdlib.h>

void mx_timers_init(mx_timers *ts) { ts->t = NULL; ts->n = ts->cap = 0; }
void mx_timers_free(mx_timers *ts) { free(ts->t); mx_timers_init(ts); }

size_t mx_timers_add(mx_timers *ts, int64_t every_ms, int64_t first_at, mx_timer_fn fn, void *ctx) {
    if (ts->n == ts->cap) {
        ts->cap = ts->cap ? ts->cap * 2 : 8;
        ts->t = xrealloc(ts->t, ts->cap * sizeof *ts->t);
    }
    if (every_ms < 1) every_ms = 1;
    mx_timer *t = &ts->t[ts->n];
    t->fn = fn;
    t->ctx = ctx;
    t->every_ms = every_ms;
    t->next_at = first_at ? first_at : mono_ms() + every_ms;
    return ts->n++;
}

size_t mx_timers_run(mx_timers *ts, int64_t now) {
    size_t ran = 0;
    for (size_t i = 0; i < ts->n; i++) {
        mx_timer *t = &ts->t[i];
        if (now < t->next_at) continue;
        t->next_at += t->every_ms;
        if (t->next_at <= now) t->next_at = now + t->every_ms; /* fell behind: skip missed runs */
        t->fn(t->ctx, now);
        ran++;
    }
    return ran;
}

int64_t mx_timers_next_in(const mx_timers *ts, int64_t now) {
    int64_t best = -1;
    for (size_t i = 0; i < ts->n; i++) {
        int64_t d = ts->t[i].next_at - now;
        if (d < 0) d = 0;
        if (best < 0 || d < best) best = d;
    }
    return best;
}
