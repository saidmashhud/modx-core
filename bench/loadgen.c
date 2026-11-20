/* loadgen — closed-loop HTTP/1.1 load generator for the modx-core benchmarks.
 *
 *   loadgen --port P [--host 127.0.0.1] --mode get|list|write|create|mixed
 *           [--conns 64] [--duration 10] [--rows 100000]
 *   loadgen --port P --mode sse --subs 1000 [--writes 200]
 *
 * Request modes keep one request in flight per connection (keep-alive) and
 * print one JSON line: requests, errors, req/s and latency percentiles (ms).
 *   get    GET /api/todos/td_seed<random id>
 *   list   GET /api/todos?list=list<random 0..999>&limit=50
 *   write  PUT /api/todos/td_seed<random id> {"done":...}
 *   create POST /api/todos
 *   mixed  80% get, 10% list, 10% write
 * sse mode opens `subs` event streams, then sends `writes` POSTs one after
 * another and measures, per subscriber, the time from sending a write until
 * its event arrived. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

typedef struct {
    double *v;
    size_t n, cap;
} samples;

static void push(samples *s, double x) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4096;
        s->v = realloc(s->v, s->cap * sizeof *s->v);
        if (!s->v) { perror("realloc"); exit(2); }
    }
    s->v[s->n++] = x;
}

static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double pct(samples *s, double p) {
    if (!s->n) return 0;
    size_t i = (size_t)(p / 100.0 * (double)(s->n - 1) + 0.5);
    return s->v[i];
}

static const char *host = "127.0.0.1";
static int port = 8080;

static int dial(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

static int write_all_blocking(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) { struct pollfd pf = { fd, POLLOUT, 0 }; poll(&pf, 1, 1000); continue; }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

typedef struct {
    int fd;
    char *buf;
    size_t len, cap;
    double sent_at;
    int busy;
} conn;

static long rows = 100000;
static const char *mode = "get";

static int build_request(char *out, size_t cap, const char *m) {
    if (strcmp(m, "mixed") == 0) {
        uint64_t r = rnd() % 10;
        m = r < 8 ? "get" : r == 8 ? "list" : "write";
    }
    long id = (long)(rnd() % (uint64_t)rows);
    if (strcmp(m, "get") == 0)
        return snprintf(out, cap, "GET /api/todos/td_seed%07ld HTTP/1.1\r\nHost: b\r\n\r\n", id);
    if (strcmp(m, "list") == 0)
        return snprintf(out, cap, "GET /api/todos?list=list%03ld&limit=50 HTTP/1.1\r\nHost: b\r\n\r\n",
                        (long)(rnd() % 1000));
    if (strcmp(m, "write") == 0) {
        const char *body = (rnd() & 1) ? "{\"done\":true}" : "{\"done\":false}";
        return snprintf(out, cap,
                        "PUT /api/todos/td_seed%07ld HTTP/1.1\r\nHost: b\r\nContent-Type: application/json\r\n"
                        "Content-Length: %zu\r\n\r\n%s", id, strlen(body), body);
    }
    char body[96];
    int bl = snprintf(body, sizeof body, "{\"title\":\"bench create %ld\",\"list\":\"list%03ld\"}", id, id % 1000);
    return snprintf(out, cap,
                    "POST /api/todos HTTP/1.1\r\nHost: b\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                    bl, body);
}

/* Complete response at the start of buf? Returns its length (0 = incomplete); *status set. */
static size_t response_len(const char *buf, size_t len, int *status) {
    const char *end = NULL;
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { end = buf + i; break; }
    if (!end) return 0;
    size_t head = (size_t)(end - buf) + 4;
    *status = atoi(buf + 9);
    long cl = 0;
    for (const char *p = buf; p < end; p++) {
        if ((*p == 'C' || *p == 'c') && strncasecmp(p, "Content-Length:", 15) == 0) { cl = strtol(p + 15, NULL, 10); break; }
    }
    if (len < head + (size_t)cl) return 0;
    return head + (size_t)cl;
}

static void ensure(conn *c, size_t extra) {
    if (c->len + extra + 1 > c->cap) {
        c->cap = (c->len + extra + 1) * 2;
        c->buf = realloc(c->buf, c->cap);
        if (!c->buf) { perror("realloc"); exit(2); }
    }
}

static int run_requests(int nconns, double duration) {
    conn *cs = calloc((size_t)nconns, sizeof *cs);
    struct pollfd *pf = calloc((size_t)nconns, sizeof *pf);
    samples lat = { 0 };
    long errors = 0, non2xx = 0;
    char req[512];
    for (int i = 0; i < nconns; i++) {
        cs[i].fd = dial();
        if (cs[i].fd < 0) { fprintf(stderr, "loadgen: connect failed: %s\n", strerror(errno)); return 2; }
    }
    double t0 = now_s(), stop_at = t0 + duration;
    long done = 0;
    for (;;) {
        double now = now_s();
        int active = 0;
        for (int i = 0; i < nconns; i++) {
            conn *c = &cs[i];
            if (c->fd < 0) continue;
            if (!c->busy && now < stop_at) {
                int n = build_request(req, sizeof req, mode);
                c->sent_at = now_s();
                if (write_all_blocking(c->fd, req, (size_t)n) != 0) { errors++; close(c->fd); c->fd = dial(); continue; }
                c->busy = 1;
            }
            if (c->busy) active++;
            pf[i].fd = c->fd;
            pf[i].events = POLLIN;
            pf[i].revents = 0;
        }
        if (!active) break;
        int rc = poll(pf, (nfds_t)nconns, 1000);
        if (rc < 0 && errno != EINTR) { perror("poll"); break; }
        for (int i = 0; i < nconns; i++) {
            conn *c = &cs[i];
            if (c->fd < 0 || !(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            for (;;) {
                ensure(c, 65536);
                ssize_t r = read(c->fd, c->buf + c->len, 65536);
                if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (r <= 0) { errors++; close(c->fd); c->fd = dial(); c->busy = 0; c->len = 0; break; }
                c->len += (size_t)r;
                int status = 0;
                size_t n = response_len(c->buf, c->len, &status);
                if (n) {
                    push(&lat, (now_s() - c->sent_at) * 1000.0);
                    if (status < 200 || status >= 300) non2xx++;
                    done++;
                    memmove(c->buf, c->buf + n, c->len - n);
                    c->len -= n;
                    c->busy = 0;
                    break;
                }
            }
        }
    }
    double elapsed = now_s() - t0;
    qsort(lat.v, lat.n, sizeof *lat.v, cmpd);
    printf("{\"mode\":\"%s\",\"conns\":%d,\"duration_s\":%.2f,\"requests\":%ld,\"errors\":%ld,\"non2xx\":%ld,"
           "\"rps\":%.0f,\"p50_ms\":%.3f,\"p95_ms\":%.3f,\"p99_ms\":%.3f,\"max_ms\":%.3f}\n",
           mode, nconns, elapsed, done, errors, non2xx, (double)done / elapsed, pct(&lat, 50), pct(&lat, 95),
           pct(&lat, 99), lat.n ? lat.v[lat.n - 1] : 0.0);
    return 0;
}

/* ---------------- SSE fan-out ---------------- */

static int run_sse(int subs, int writes) {
    conn *cs = calloc((size_t)subs, sizeof *cs);
    struct pollfd *pf = calloc((size_t)subs, sizeof *pf);
    double *sent = calloc((size_t)writes, sizeof *sent);
    int *got = calloc((size_t)subs, sizeof *got);
    samples lat = { 0 }, wlat = { 0 };
    const char *open_req = "GET /api/events HTTP/1.1\r\nHost: b\r\nAccept: text/event-stream\r\n\r\n";
    double t_open = now_s();
    for (int i = 0; i < subs; i++) {
        cs[i].fd = dial();
        if (cs[i].fd < 0 || write_all_blocking(cs[i].fd, open_req, strlen(open_req)) != 0) {
            fprintf(stderr, "loadgen: subscriber %d failed: %s\n", i, strerror(errno));
            return 2;
        }
    }
    /* wait until every stream answered with its headers */
    int ready = 0;
    while (ready < subs && now_s() - t_open < 30) {
        for (int i = 0; i < subs; i++) { pf[i].fd = cs[i].fd; pf[i].events = POLLIN; pf[i].revents = 0; }
        poll(pf, (nfds_t)subs, 100);
        for (int i = 0; i < subs; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            conn *c = &cs[i];
            ensure(c, 4096);
            ssize_t r = read(c->fd, c->buf + c->len, 4096);
            if (r > 0) {
                c->len += (size_t)r;
                c->buf[c->len] = 0;
                if (!c->busy && strstr(c->buf, "\r\n\r\n")) { c->busy = 1; ready++; c->len = 0; }
            }
        }
    }
    double open_ms = (now_s() - t_open) * 1000.0;
    if (ready < subs) { fprintf(stderr, "loadgen: only %d/%d streams opened\n", ready, subs); return 2; }
    int wfd = dial();
    char req[512], resp[8192];
    double t0 = now_s();
    long delivered = 0;
    for (int k = 0; k < writes; k++) {
        char body[64];
        int bl = snprintf(body, sizeof body, "{\"title\":\"bench-%d\"}", k);
        int n = snprintf(req, sizeof req,
                         "POST /api/todos HTTP/1.1\r\nHost: b\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                         bl, body);
        sent[k] = now_s();
        write_all_blocking(wfd, req, (size_t)n);
        size_t rl = 0;
        int status = 0;
        for (;;) {
            struct pollfd p = { wfd, POLLIN, 0 };
            poll(&p, 1, 1000);
            ssize_t r = read(wfd, resp + rl, sizeof resp - rl - 1);
            if (r > 0) rl += (size_t)r;
            else if (r == 0 || (errno != EAGAIN && errno != EINTR)) break;
            if (response_len(resp, rl, &status)) break;
        }
        push(&wlat, (now_s() - sent[k]) * 1000.0);
        /* drain subscribers without blocking */
        for (int i = 0; i < subs; i++) { pf[i].fd = cs[i].fd; pf[i].events = POLLIN; pf[i].revents = 0; }
        poll(pf, (nfds_t)subs, 0);
        for (int i = 0; i < subs; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            conn *c = &cs[i];
            for (;;) {
                ensure(c, 65536);
                ssize_t r = read(c->fd, c->buf + c->len, 65536);
                if (r <= 0) break;
                double t = now_s();
                c->len += (size_t)r;
                c->buf[c->len] = 0;
                char *p = c->buf, *last = c->buf;
                while ((p = strstr(p, "\"title\":\"bench-"))) {
                    int idx = atoi(p + 15);
                    char *q = strchr(p + 15, '"');
                    if (!q) break;
                    if (idx >= 0 && idx < writes) { push(&lat, (t - sent[idx]) * 1000.0); got[i]++; delivered++; }
                    p = last = q;
                }
                size_t keep = c->len - (size_t)(last - c->buf);
                if (keep > 256) keep = 256; /* a partial marker is shorter than this */
                memmove(c->buf, c->buf + c->len - keep, keep);
                c->len = keep;
            }
        }
    }
    /* wait for stragglers */
    double deadline = now_s() + 10;
    while (delivered < (long)subs * writes && now_s() < deadline) {
        for (int i = 0; i < subs; i++) { pf[i].fd = cs[i].fd; pf[i].events = POLLIN; pf[i].revents = 0; }
        if (poll(pf, (nfds_t)subs, 200) <= 0) continue;
        for (int i = 0; i < subs; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            conn *c = &cs[i];
            ensure(c, 65536);
            ssize_t r = read(c->fd, c->buf + c->len, 65536);
            if (r <= 0) continue;
            double t = now_s();
            c->len += (size_t)r;
            c->buf[c->len] = 0;
            char *p = c->buf, *last = c->buf;
            while ((p = strstr(p, "\"title\":\"bench-"))) {
                int idx = atoi(p + 15);
                char *q = strchr(p + 15, '"');
                if (!q) break;
                if (idx >= 0 && idx < writes) { push(&lat, (t - sent[idx]) * 1000.0); got[i]++; delivered++; }
                p = last = q;
            }
            size_t keep = c->len - (size_t)(last - c->buf);
            if (keep > 256) keep = 256;
            memmove(c->buf, c->buf + c->len - keep, keep);
            c->len = keep;
        }
    }
    double elapsed = now_s() - t0;
    qsort(lat.v, lat.n, sizeof *lat.v, cmpd);
    qsort(wlat.v, wlat.n, sizeof *wlat.v, cmpd);
    printf("{\"mode\":\"sse\",\"subs\":%d,\"writes\":%d,\"open_ms\":%.1f,\"expected\":%ld,\"delivered\":%ld,"
           "\"elapsed_s\":%.2f,\"events_per_s\":%.0f,\"deliver_p50_ms\":%.3f,\"deliver_p95_ms\":%.3f,"
           "\"deliver_p99_ms\":%.3f,\"write_p50_ms\":%.3f,\"write_p99_ms\":%.3f}\n",
           subs, writes, open_ms, (long)subs * writes, delivered, elapsed, (double)delivered / elapsed,
           pct(&lat, 50), pct(&lat, 95), pct(&lat, 99), pct(&wlat, 50), pct(&wlat, 99));
    return delivered == (long)subs * writes ? 0 : 1;
}

int main(int argc, char **argv) {
    int conns = 64, subs = 1000, writes = 200;
    double duration = 10;
    for (int i = 1; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "--host")) host = v;
        else if (!strcmp(k, "--port")) port = atoi(v);
        else if (!strcmp(k, "--mode")) mode = v;
        else if (!strcmp(k, "--conns")) conns = atoi(v);
        else if (!strcmp(k, "--duration")) duration = atof(v);
        else if (!strcmp(k, "--rows")) rows = atol(v);
        else if (!strcmp(k, "--subs")) subs = atoi(v);
        else if (!strcmp(k, "--writes")) writes = atoi(v);
        else { fprintf(stderr, "loadgen: unknown option %s\n", k); return 2; }
    }
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max < 16384 ? rl.rlim_max : 16384;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    signal(SIGPIPE, SIG_IGN);
    rs ^= (uint64_t)getpid() << 17;
    if (rows < 1) rows = 1;
    if (!strcmp(mode, "sse")) return run_sse(subs, writes);
    return run_requests(conns, duration);
}
