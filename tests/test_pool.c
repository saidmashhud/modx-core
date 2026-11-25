/* Worker pool: jobs run on workers, completions on the thread that runs
 * them, in bounded queues. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "modx/pool.h"
#include "test.h"

typedef struct {
    int id;
    unsigned long acc;        /* written by the worker */
    pthread_t worker_thread;  /* written by the worker */
    int done_on_main;         /* written by done */
    int *order;               /* completion log */
    int *norder;
} job_arg;

static pthread_t main_thread;

static void busy_work(void *vp) {
    job_arg *a = vp;
    unsigned long x = (unsigned long)a->id + 1;
    for (int i = 0; i < 20000; i++) x = x * 6364136223846793005UL + 1442695040888963407UL;
    a->acc = x;
    a->worker_thread = pthread_self();
}

static void busy_done(void *vp) {
    job_arg *a = vp;
    a->done_on_main = pthread_equal(pthread_self(), main_thread) && !pthread_equal(a->worker_thread, main_thread);
    a->order[(*a->norder)++] = a->id;
}

static void t_pool_basic(void) {
    main_thread = pthread_self();
    mx_pool *p = mx_pool_new(4, 0);
    CHECK(p != NULL);
    if (!p) return;
    enum { N = 300 };
    static job_arg args[N];
    int order[N], norder = 0;
    for (int i = 0; i < N; i++) {
        args[i] = (job_arg){ i, 0, main_thread, 0, order, &norder };
        CHECK(mx_pool_submit(p, busy_work, busy_done, &args[i]) == 0);
    }
    for (int spins = 0; norder < N && spins < 10000; spins++) mx_pool_wait(p, 100);
    CHECK(norder == N);
    int on_main = 1, distinct = 1;
    for (int i = 0; i < N; i++) {
        if (!args[i].done_on_main || args[i].acc == 0) on_main = 0;
        int seen = 0;
        for (int k = 0; k < norder; k++) seen += order[k] == i;
        if (seen != 1) distinct = 0;
    }
    CHECK(on_main);
    CHECK(distinct); /* every done exactly once */
    mx_pool_stats st;
    mx_pool_stats_get(p, &st);
    CHECK(st.threads == 4 && st.submitted == N && st.completed == N && st.queued == 0 && st.running == 0);
    CHECK(mx_pool_busy(p) == 0);
    CHECK(mx_pool_run_done(p) == 0);
    mx_pool_free(p);
}

static pthread_mutex_t gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int gate_open;
static int gate_done;

static void gated(void *vp) {
    (void)vp;
    pthread_mutex_lock(&gate_mu);
    while (!gate_open) pthread_cond_wait(&gate_cv, &gate_mu);
    pthread_mutex_unlock(&gate_mu);
}
static void gated_done(void *vp) { (void)vp; gate_done++; }

/* bounded queue, refused submits, free() finishes what was queued */
static void t_pool_bounds(void) {
    mx_pool *p = mx_pool_new(1, 2);
    CHECK(p != NULL);
    if (!p) return;
    gate_open = 0;
    gate_done = 0;
    CHECK(mx_pool_submit(p, gated, gated_done, NULL) == 0); /* the worker takes it and blocks */
    while (mx_pool_busy(p) != 1) usleep(1000);
    mx_pool_stats st;
    for (int i = 0; i < 100; i++) {
        mx_pool_stats_get(p, &st);
        if (st.running == 1) break;
        usleep(1000);
    }
    CHECK(st.running == 1);
    CHECK(mx_pool_submit(p, gated, gated_done, NULL) == 0);
    CHECK(mx_pool_submit(p, gated, gated_done, NULL) == 0);
    errno = 0;
    CHECK(mx_pool_submit(p, gated, gated_done, NULL) == -1 && errno == EAGAIN);
    mx_pool_stats_get(p, &st);
    CHECK(st.rejected == 1 && st.queued == 2);
    CHECK(mx_pool_wait(p, 20) == 0); /* nothing finished yet */
    pthread_mutex_lock(&gate_mu);
    gate_open = 1;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&gate_mu);
    mx_pool_free(p); /* joins after the 3 queued jobs, runs their done */
    CHECK(gate_done == 3);
    mx_pool_free(NULL);
}

/* a forked child must not use the pool (its workers do not exist there) */
static void t_pool_fork_child(void) {
    mx_pool *p = mx_pool_new(1, 0);
    CHECK(p != NULL);
    if (!p) return;
    pid_t pid = fork();
    if (pid == 0) {
        int rc = mx_pool_submit(p, gated, NULL, NULL);
        _exit(rc == -1 && errno == ECHILD ? 0 : 1);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    mx_pool_free(p);
}

void test_pool_all(void) {
    RUN(t_pool_basic);
    RUN(t_pool_bounds);
    RUN(t_pool_fork_child);
}
