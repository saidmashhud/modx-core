/* Fuzz target: store recovery. Input = [snapshot.json 0x01] journal lines.
 * Each journal line gets a correct CRC prefix unless it starts with '#'
 * (then it is written raw), so both valid records and corrupt ones reach
 * the replay. Opening must never crash, and a store that opened must take a
 * new record and reopen to the same state plus that record. A journal that is
 * refused is opened again in salvage mode, which must succeed whenever the
 * snapshot parses and leave a store that reopens cleanly without salvage. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modx/store.h"

static const mx_coll_def DEFS[2] = { { "items", 0, NULL }, { "users", MX_COLL_REDACT, NULL } };
static char dir[256];

static void put_file(const char *name, const uint8_t *p, size_t n) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) abort();
    if (n) fwrite(p, 1, n, f);
    fclose(f);
}

static void rm_all(void) {
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        unlink(path);
    }
    if (d) closedir(d);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!dir[0]) {
        const char *t = getenv("TMPDIR");
        snprintf(dir, sizeof dir, "%s/modx-fuzz-journal-%d", t && *t ? t : "/tmp", (int)getpid());
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) abort();
    }
    rm_all();
    const uint8_t *z = memchr(data, 1, size);
    const uint8_t *jr = data;
    size_t jn = size;
    if (z) {
        put_file("snapshot.json", data, (size_t)(z - data));
        jr = z + 1;
        jn = size - (size_t)(z - data) - 1;
    }
    sbuf j;
    sbuf_init(&j);
    size_t pos = 0;
    while (pos < jn) {
        const uint8_t *nl = memchr(jr + pos, '\n', jn - pos);
        size_t len = nl ? (size_t)(nl - (jr + pos)) : jn - pos;
        if (len && jr[pos] == '#') sbuf_put(&j, jr + pos + 1, len - 1);
        else {
            sbuf_printf(&j, "%08x ", crc32_buf(jr + pos, len));
            sbuf_put(&j, jr + pos, len);
        }
        if (nl) sbuf_putc(&j, '\n');
        pos += len + (nl ? 1 : 0);
    }
    put_file("journal.log", (const uint8_t *)j.data, j.len);
    sbuf_free(&j);
    char err[256];
    mx_store *st = mx_store_open(dir, DEFS, 2, 64, NULL, err, sizeof err);
    if (!st) {
        if (strstr(err, "corrupt snapshot") && !strstr(err, "checksum")) return 0; /* never half-loaded */
        /* damage that is not a torn tail: salvage keeps what is valid */
        char rp[600];
        snprintf(rp, sizeof rp, "%s/report.txt", dir);
        mx_store_opts o = { 1, rp };
        st = mx_store_open_ex(dir, DEFS, 2, 64, NULL, &o, err, sizeof err);
        if (!st) {
            if (strstr(err, "corrupt snapshot")) return 0;
            fprintf(stderr, "salvage failed: %s\n", err);
            abort();
        }
        mx_store_close(st);
        st = mx_store_open(dir, DEFS, 2, 64, NULL, err, sizeof err);
        if (!st) { fprintf(stderr, "reopen after salvage failed: %s\n", err); abort(); }
    }
    int64_t seq = st->seq;
    size_t n0 = mx_store_count(st, 0), n1 = mx_store_count(st, 1);
    int had = mx_store_get(st, 0, "fuzz-added") != NULL;
    json *o = json_obj();
    json_set(o, "id", json_str("fuzz-added"));
    if (mx_store_put(st, 0, o, NULL) != 0 || mx_store_sync(st) != 0) abort();
    mx_store_close(st);
    st = mx_store_open(dir, DEFS, 2, 64, NULL, err, sizeof err);
    if (!st || st->seq != seq + 1 || mx_store_count(st, 1) != n1) abort();
    if (!mx_store_get(st, 0, "fuzz-added") || mx_store_count(st, 0) != n0 + (had ? 0 : 1)) abort();
    mx_store_close(st);
    return 0;
}
