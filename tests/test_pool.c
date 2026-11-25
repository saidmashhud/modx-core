/* Worker pool: jobs run on workers, completions on the thread that runs
 * them, in bounded queues; async password hashing / verification on it. */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "modx/crypto.h"
#include "modx/pool.h"
#include "modx/store.h"
#include "test.h"
#include "test_util.h"

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

typedef struct {
    char *hash;
    int calls, ok, rehash;
    int on_main;
} pw_result;

static void got_hash(void *ctx, char *hash) {
    pw_result *r = ctx;
    r->hash = hash;
    r->calls++;
    r->on_main = pthread_equal(pthread_self(), main_thread);
}

static void got_verify(void *ctx, int ok, int rehash) {
    pw_result *r = ctx;
    r->ok = ok;
    r->rehash = rehash;
    r->calls++;
    r->on_main = pthread_equal(pthread_self(), main_thread);
}

static void wait_calls(mx_pool *p, pw_result *r, int n) {
    for (int i = 0; i < 2000; i++) {
        int total = 0;
        for (int k = 0; k < n; k++) total += r[k].calls;
        if (total >= n) return;
        mx_pool_wait(p, 50);
    }
}

static void t_password_async(void) {
    main_thread = pthread_self();
    mx_pool *p = mx_pool_new(3, 0);
    CHECK(p != NULL);
    if (!p) return;
    char pw[32] = "async horse";
    pw_result h = { 0 };
    CHECK(password_hash_async(p, pw, 2000, got_hash, &h) == 0);
    memset(pw, 'x', sizeof pw - 1); /* the job has its own copy */
    wait_calls(p, &h, 1);
    CHECK(h.calls == 1 && h.on_main && h.hash && strncmp(h.hash, "pbkdf2_sha256$2000$", 19) == 0);
    CHECK(h.hash && password_verify("async horse", h.hash)); /* the sync API agrees */
    /* many verifications at once, right and wrong, on 3 workers */
    enum { N = 24 };
    pw_result v[N];
    memset(v, 0, sizeof v);
    for (int i = 0; i < N; i++)
        CHECK(password_verify_async(p, i % 3 ? "async horse" : "async horsf", h.hash, got_verify, &v[i]) == 0);
    wait_calls(p, v, N);
    int right = 1;
    for (int i = 0; i < N; i++)
        if (v[i].calls != 1 || !v[i].on_main || v[i].ok != (i % 3 != 0) || v[i].rehash != (i % 3 != 0)) right = 0;
    CHECK(right); /* 2000 < PASSWORD_ITERS: a match asks for a rehash */
    /* a malformed stored string fails without work, still through the pool */
    pw_result bad = { 0 };
    CHECK(password_verify_async(p, "x", "plain", got_verify, &bad) == 0);
    wait_calls(p, &bad, 1);
    CHECK(bad.calls == 1 && bad.ok == 0 && bad.rehash == 0);
    /* default iterations */
    pw_result d = { 0 };
    CHECK(password_hash_async(p, "default", 0, got_hash, &d) == 0);
    wait_calls(p, &d, 1);
    CHECK(d.hash && strncmp(d.hash, "pbkdf2_sha256$600000$", 21) == 0);
    pw_result dv = { 0 };
    CHECK(d.hash && password_verify_async(p, "default", d.hash, got_verify, &dv) == 0);
    wait_calls(p, &dv, 1);
    CHECK(dv.ok == 1 && dv.rehash == 0);
    free(d.hash);
    free(h.hash);
    mx_pool_free(p);
}

/* The store's background compaction forks while workers are inside PBKDF2: the
 * child only serializes and writes (workers hold no lock it needs) and the
 * snapshot it writes is complete. */
static void t_fork_compaction_while_hashing(void) {
    main_thread = pthread_self();
    static const mx_coll_def defs[] = { { "rows", 0, NULL } };
    char *dir = make_tmpdir();
    char err[256];
    mx_store *st = mx_store_open(dir, defs, 1, 16, NULL, err, sizeof err);
    mx_pool *p = mx_pool_new(4, 0);
    CHECK(st && p);
    if (!st || !p) { mx_store_close(st); mx_pool_free(p); rm_rf(dir); return; }
    enum { N = 16 };
    pw_result v[N];
    memset(v, 0, sizeof v);
    for (int i = 0; i < N; i++) CHECK(password_hash_async(p, "fork", 30000, got_hash, &v[i]) == 0);
    int compactions = 0;
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < 50; i++) {
            char id[16];
            snprintf(id, sizeof id, "r%d-%d", round, i);
            json *o = json_obj();
            json_set(o, "id", json_str(id));
            mx_store_put(st, 0, o, NULL);
        }
        CHECK(mx_store_sync(st) == 0);
        if (mx_store_compact_bg(st) == 0) compactions++;
        for (int k = 0; k < 400 && st->compact_pid > 0; k++) { mx_store_tick(st); usleep(5000); }
        mx_pool_run_done(p);
    }
    wait_calls(p, v, N);
    int all = 1;
    for (int i = 0; i < N; i++) {
        if (v[i].calls != 1 || !v[i].hash || !password_verify("fork", v[i].hash)) all = 0;
        free(v[i].hash);
    }
    CHECK(all);
    CHECK(compactions == 8 && st->compactions == 8 && st->compact_failures == 0);
    mx_store_close(st);
    st = mx_store_open(dir, defs, 1, 16, NULL, err, sizeof err);
    CHECK(st && mx_store_count(st, 0) == 400 && st->seq == 400);
    mx_store_close(st);
    mx_pool_free(p);
    rm_rf(dir);
}

void test_pool_all(void) {
    RUN(t_pool_basic);
    RUN(t_pool_bounds);
    RUN(t_pool_fork_child);
    RUN(t_password_async);
    RUN(t_fork_compaction_while_hashing);
}
