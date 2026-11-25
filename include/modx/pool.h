#ifndef MODX_POOL_H
#define MODX_POOL_H

/** @file pool.h
 * @brief Worker thread pool whose completions run on the event-loop thread.
 *
 * For CPU-bound work that must not block the single loop thread (password
 * hashing, see password_hash_async()). A job's @c work function runs on a
 * worker thread; its @c done function runs later on the thread that calls
 * mx_pool_run_done(), normally the loop thread: with mx_pool_attach() the
 * HTTP server does that as soon as a worker signals a completion (a pipe
 * registered with the server's poller wakes the loop).
 *
 * Rules for @c work functions: touch only the job's own memory. No store,
 * HTTP or other loop-owned state, no stdio, and preferably no malloc (then
 * a worker never holds a libc lock when the loop thread forks: the store's
 * background compaction forks, and its child only serializes and writes the
 * snapshot; it never touches the pool). The pool's own mutex is never taken
 * in a forked child; mx_pool_submit() refuses to run there.
 */

#include <stddef.h>
#include <stdint.h>

/** Opaque worker pool (mx_pool_new()). */
typedef struct mx_pool mx_pool;
/** HTTP server (modx/http.h), for mx_pool_attach(). */
typedef struct http_server http_server;

/** Runs on a worker thread; see the rules for @c work functions above. */
typedef void (*mx_job_fn)(void *arg);
/** Runs on the thread that calls mx_pool_run_done() (the loop thread). */
typedef void (*mx_done_fn)(void *arg);

#define MX_POOL_THREADS 2       /**< default worker count */
#define MX_POOL_MAX_QUEUE 1024  /**< default bound on jobs waiting for a worker */

/** Starts @p nthreads workers (<= 0: MX_POOL_THREADS) with at most
 * @p max_queue jobs waiting (0: MX_POOL_MAX_QUEUE). NULL when no thread or
 * the wake-up pipe cannot be created (if only some threads start, the pool
 * runs with those; see mx_pool_stats::threads). Free with mx_pool_free(). */
mx_pool *mx_pool_new(int nthreads, size_t max_queue);

/** Queues a job (call from the loop thread). @p work runs on a worker,
 * then @p done (may be NULL) on the completion thread; @p arg is passed to
 * both and stays the caller's (free it in done).
 * @return 0, or -1 with errno = EAGAIN (queue full), EPIPE (pool stopping)
 * or ECHILD (called in a forked child); then neither function will run. */
int mx_pool_submit(mx_pool *p, mx_job_fn work, mx_done_fn done, void *arg);

/** Runs the @c done callbacks of finished jobs, in completion order, on the
 * calling thread. Non-blocking. @return how many ran. */
int mx_pool_run_done(mx_pool *p);

/** Waits up to @p timeout_ms (-1: forever) for at least one completion, then
 * runs mx_pool_run_done(). For tools and tests without an event loop.
 * @return how many callbacks ran (0 on timeout). */
int mx_pool_wait(mx_pool *p, int timeout_ms);

/** Readable while completions are waiting (for a custom event loop; then call
 * mx_pool_run_done() when it is). */
int mx_pool_fd(const mx_pool *p);

/** Lets @p srv run the completions: registers mx_pool_fd() with
 * http_server_watch(). @return 0 or -1. */
int mx_pool_attach(mx_pool *p, http_server *srv);

/** Jobs queued, running or finished but not yet handed to their @c done.
 * Takes the pool mutex. */
size_t mx_pool_busy(const mx_pool *p);

/** Statistics (mx_pool_stats_get()). */
typedef struct {
    int threads;                            /**< workers running */
    int64_t submitted, completed, rejected; /**< jobs accepted / whose done ran / refused by mx_pool_submit() */
    size_t queued, running;                 /**< jobs waiting for a worker / on a worker now */
} mx_pool_stats;
/** Fills @p s (a consistent snapshot, taken under the pool mutex). */
void mx_pool_stats_get(mx_pool *p, mx_pool_stats *s);

/** Stops accepting jobs, lets the workers finish every queued job, joins
 * them, runs the remaining @c done callbacks on the calling thread and frees
 * the pool. Call it after http_server_close() (detach first with
 * http_server_unwatch() if the server lives on). NULL is a no-op. */
void mx_pool_free(mx_pool *p);

#endif
