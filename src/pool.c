#include "modx/pool.h"
#include "modx/http.h"
#include "modx/util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct mx_job {
    mx_job_fn work;
    mx_done_fn done;
    void *arg;
    struct mx_job *next;
} mx_job;

struct mx_pool {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    mx_job *qhead, *qtail;   /* waiting for a worker */
    mx_job *dhead, *dtail;   /* finished, waiting for mx_pool_run_done */
    size_t queued, running, max_queue;
    int stop;
    int nthreads;
    pthread_t *threads;
    int wake_r, wake_w;      /* a byte in the pipe = completions waiting */
    int wake_pending;        /* a byte was written and not consumed yet */
    pid_t owner;             /* process that created the pool (a forked child must not use it) */
    int64_t submitted, completed, rejected;
};

static void *worker(void *vp) {
    mx_pool *p = vp;
    pthread_mutex_lock(&p->mu);
    for (;;) {
        while (!p->qhead && !p->stop) pthread_cond_wait(&p->cv, &p->mu);
        if (!p->qhead) break; /* stopping and nothing left */
        mx_job *j = p->qhead;
        p->qhead = j->next;
        if (!p->qhead) p->qtail = NULL;
        p->queued--;
        p->running++;
        pthread_mutex_unlock(&p->mu);
        j->work(j->arg);
        pthread_mutex_lock(&p->mu);
        p->running--;
        j->next = NULL;
        if (p->dtail) p->dtail->next = j;
        else p->dhead = j;
        p->dtail = j;
        if (!p->wake_pending) {
            p->wake_pending = 1;
            ssize_t w;
            do w = write(p->wake_w, "x", 1); while (w < 0 && errno == EINTR);
        }
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

static int pipe_cloexec_nonblock(int fds[2]) {
    if (pipe(fds) != 0) return -1;
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(fds[i], F_GETFL, 0);
        if (fl < 0 || fcntl(fds[i], F_SETFL, fl | O_NONBLOCK) != 0 || fcntl(fds[i], F_SETFD, FD_CLOEXEC) != 0) {
            close(fds[0]);
            close(fds[1]);
            return -1;
        }
    }
    return 0;
}

mx_pool *mx_pool_new(int nthreads, size_t max_queue) {
    mx_pool *p = xcalloc(1, sizeof *p);
    int fds[2];
    if (pipe_cloexec_nonblock(fds) != 0) { free(p); return NULL; }
    p->wake_r = fds[0];
    p->wake_w = fds[1];
    p->max_queue = max_queue ? max_queue : MX_POOL_MAX_QUEUE;
    p->owner = getpid();
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    int n = nthreads > 0 ? nthreads : MX_POOL_THREADS;
    p->threads = xcalloc((size_t)n, sizeof *p->threads);
    for (int i = 0; i < n; i++) {
        if (pthread_create(&p->threads[i], NULL, worker, p) != 0) break;
        p->nthreads++;
    }
    if (p->nthreads == 0) { mx_pool_free(p); return NULL; }
    return p;
}

int mx_pool_submit(mx_pool *p, mx_job_fn work, mx_done_fn done, void *arg) {
    if (getpid() != p->owner) { errno = ECHILD; return -1; }
    mx_job *j = xmalloc(sizeof *j); /* allocated here, on the loop thread: workers never malloc for the pool */
    j->work = work;
    j->done = done;
    j->arg = arg;
    j->next = NULL;
    pthread_mutex_lock(&p->mu);
    int err = p->stop ? EPIPE : p->queued >= p->max_queue ? EAGAIN : 0;
    if (err) {
        p->rejected++;
        pthread_mutex_unlock(&p->mu);
        free(j);
        errno = err;
        return -1;
    }
    if (p->qtail) p->qtail->next = j;
    else p->qhead = j;
    p->qtail = j;
    p->queued++;
    p->submitted++;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

int mx_pool_run_done(mx_pool *p) {
    /* consume the wake-up byte BEFORE taking the list: a worker that finishes
     * after this point writes a new byte, so no completion is ever left behind
     * without one */
    char buf[64];
    while (read(p->wake_r, buf, sizeof buf) > 0) {}
    pthread_mutex_lock(&p->mu);
    mx_job *j = p->dhead;
    p->dhead = p->dtail = NULL;
    p->wake_pending = 0;
    pthread_mutex_unlock(&p->mu);
    int n = 0;
    while (j) {
        mx_job *next = j->next;
        if (j->done) j->done(j->arg);
        free(j);
        j = next;
        n++;
    }
    if (n) {
        pthread_mutex_lock(&p->mu);
        p->completed += n;
        pthread_mutex_unlock(&p->mu);
    }
    return n;
}

int mx_pool_wait(mx_pool *p, int timeout_ms) {
    struct pollfd pf = { p->wake_r, POLLIN, 0 };
    int rc;
    do rc = poll(&pf, 1, timeout_ms); while (rc < 0 && errno == EINTR);
    return mx_pool_run_done(p);
}

int mx_pool_fd(const mx_pool *p) { return p->wake_r; }

static void on_wake(http_server *srv, int fd, void *ctx) {
    (void)srv;
    (void)fd;
    mx_pool_run_done(ctx);
}

int mx_pool_attach(mx_pool *p, http_server *srv) { return http_server_watch(srv, p->wake_r, on_wake, p); }

size_t mx_pool_busy(const mx_pool *p) {
    mx_pool *m = (mx_pool *)p;
    pthread_mutex_lock(&m->mu);
    size_t n = m->queued + m->running;
    for (mx_job *j = m->dhead; j; j = j->next) n++;
    pthread_mutex_unlock(&m->mu);
    return n;
}

void mx_pool_stats_get(mx_pool *p, mx_pool_stats *s) {
    pthread_mutex_lock(&p->mu);
    s->threads = p->nthreads;
    s->submitted = p->submitted;
    s->completed = p->completed;
    s->rejected = p->rejected;
    s->queued = p->queued;
    s->running = p->running;
    pthread_mutex_unlock(&p->mu);
}

void mx_pool_free(mx_pool *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->stop = 1;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < p->nthreads; i++) pthread_join(p->threads[i], NULL);
    mx_pool_run_done(p); /* the application frees its job contexts in done */
    free(p->threads);
    close(p->wake_r);
    close(p->wake_w);
    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
    free(p);
}
