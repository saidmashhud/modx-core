#ifndef MODX_TEST_UTIL_H
#define MODX_TEST_UTIL_H

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modx/util.h"

static inline char *make_tmpdir(void) {
    const char *base = getenv("TMPDIR");
    sbuf p;
    sbuf_init(&p);
    sbuf_puts(&p, base && *base ? base : "/tmp");
    if (p.data[p.len - 1] != '/') sbuf_putc(&p, '/');
    sbuf_puts(&p, "modx-core-test-XXXXXX");
    char *dir = sbuf_take(&p);
    if (!mkdtemp(dir)) { perror("mkdtemp"); exit(2); }
    return dir;
}

/* Removes the files of a flat directory and the directory; frees dir. */
static inline void rm_rf(char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char path[2048];
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(dir);
    free(dir);
}

static inline char *file_path(const char *dir, const char *name) {
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = xmalloc(n);
    snprintf(p, n, "%s/%s", dir, name);
    return p;
}

static inline long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static inline void append_raw(const char *path, const char *data) {
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0 || write(fd, data, strlen(data)) != (ssize_t)strlen(data)) { perror("append"); exit(2); }
    close(fd);
}

#endif
