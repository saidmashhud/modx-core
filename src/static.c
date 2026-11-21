#include "modx/static.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct { const char *ext; const char *mime; } MIME[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".htm", "text/html; charset=utf-8" },
    { ".js", "text/javascript; charset=utf-8" },
    { ".mjs", "text/javascript; charset=utf-8" },
    { ".css", "text/css; charset=utf-8" },
    { ".json", "application/json; charset=utf-8" },
    { ".map", "application/json; charset=utf-8" },
    { ".webmanifest", "application/manifest+json" },
    { ".svg", "image/svg+xml" },
    { ".png", "image/png" },
    { ".jpg", "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".gif", "image/gif" },
    { ".webp", "image/webp" },
    { ".avif", "image/avif" },
    { ".ico", "image/x-icon" },
    { ".woff", "font/woff" },
    { ".woff2", "font/woff2" },
    { ".ttf", "font/ttf" },
    { ".otf", "font/otf" },
    { ".txt", "text/plain; charset=utf-8" },
    { ".xml", "application/xml" },
    { ".wasm", "application/wasm" },
    { ".pdf", "application/pdf" },
    { ".mp4", "video/mp4" },
    { ".webm", "video/webm" },
};

const char *static_mime_type(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash : path, '.');
    if (dot) {
        for (size_t i = 0; i < sizeof MIME / sizeof MIME[0]; i++)
            if (str_icase_eq(dot, MIME[i].ext)) return MIME[i].mime;
    }
    return "application/octet-stream";
}

int static_path_safe(const char *path) {
    if (path[0] != '/') return 0;
    if (strchr(path, '\\')) return 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - seg);
        if (n == 2 && seg[0] == '.' && seg[1] == '.') return 0;
        if (n == 1 && seg[0] == '.') return 0;
    }
    return 1;
}

static struct stat g_last; /* stat of the file load_file read last (single-threaded server) */

static int load_file(const char *path, sbuf *out, int *is_dir) {
    struct stat st;
    *is_dir = 0;
    if (stat(path, &st) != 0) return -1;
    g_last = st;
    if (S_ISDIR(st.st_mode)) { *is_dir = 1; return -1; }
    if (!S_ISREG(st.st_mode)) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    sbuf_reserve(out, (size_t)st.st_size);
    char buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (n == 0) break;
        sbuf_put(out, buf, (size_t)n);
    }
    close(fd);
    return 0;
}

/* Weak validator from size + mtime: adds ETag, answers 304 (returns 1) when
 * If-None-Match already names it. */
static int etag(const http_request *req, http_response *resp) {
    char tag[64];
#ifdef __APPLE__
    long long mt = (long long)g_last.st_mtimespec.tv_sec * 1000000000LL + g_last.st_mtimespec.tv_nsec;
#else
    long long mt = (long long)g_last.st_mtim.tv_sec * 1000000000LL + g_last.st_mtim.tv_nsec;
#endif
    snprintf(tag, sizeof tag, "W/\"%llx-%llx\"", (unsigned long long)g_last.st_size, (unsigned long long)mt);
    sbuf_printf(&resp->headers, "ETag: %s\r\n", tag);
    const char *inm = http_req_header(req, "if-none-match");
    if (inm && (strstr(inm, tag) || strstr(inm, tag + 2) || strcmp(inm, "*") == 0)) {
        resp->status = 304;
        resp->body.len = 0;
        if (resp->body.data) resp->body.data[0] = 0;
        resp->content_type = NULL;
        return 1;
    }
    return 0;
}

static void not_found(http_response *resp) {
    resp->status = 404;
    resp->content_type = "text/plain; charset=utf-8";
    sbuf_puts(&resp->body, "Not Found\n");
}

void static_serve(const char *root, const http_request *req, http_response *resp) {
    struct stat st;
    if (!root || stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) { not_found(resp); return; }
    if (!static_path_safe(req->path)) {
        resp->status = 400;
        resp->content_type = "text/plain; charset=utf-8";
        sbuf_puts(&resp->body, "Bad path\n");
        return;
    }
    sbuf path;
    sbuf_init(&path);
    sbuf_puts(&path, root);
    sbuf_puts(&path, req->path);
    if (path.data[path.len - 1] == '/') sbuf_puts(&path, "index.html");
    int is_dir = 0;
    const char *served = path.data;
    if (load_file(path.data, &resp->body, &is_dir) != 0) {
        if (is_dir) {
            sbuf_puts(&path, "/index.html");
            if (load_file(path.data, &resp->body, &is_dir) == 0) goto ok;
        }
        /* SPA fallback: unknown paths get the app shell — app.html when the build has
         * one (index.html is then a public start page), else index.html. Missing hashed
         * build assets under /assets/ stay 404 (HTML for a .js file only hides errors). */
        int looks_like_asset = strncmp(req->path, "/assets/", 8) == 0;
        sbuf_free(&path);
        sbuf_puts(&path, root);
        sbuf_puts(&path, "/app.html");
        served = path.data;
        int missing = looks_like_asset || load_file(path.data, &resp->body, &is_dir) != 0;
        if (missing && !looks_like_asset) {
            resp->body.len = 0;
            sbuf_free(&path);
            sbuf_puts(&path, root);
            sbuf_puts(&path, "/index.html");
            served = path.data;
            missing = load_file(path.data, &resp->body, &is_dir) != 0;
        }
        if (missing) {
            resp->body.len = 0;
            sbuf_free(&path);
            not_found(resp);
            return;
        }
        if (etag(req, resp)) { sbuf_free(&path); return; }
        resp->status = 200;
        resp->content_type = static_mime_type(served);
        sbuf_puts(&resp->headers, "Cache-Control: no-cache\r\n");
        sbuf_free(&path);
        return;
    }
ok:
    served = path.data;
    if (etag(req, resp)) { sbuf_free(&path); return; }
    resp->status = 200;
    resp->content_type = static_mime_type(served);
    if (strstr(served, "/assets/")) sbuf_puts(&resp->headers, "Cache-Control: public, max-age=31536000, immutable\r\n");
    else sbuf_puts(&resp->headers, "Cache-Control: no-cache\r\n");
    sbuf_free(&path);
}

int static_serve_exact(const char *root, const char *rel, http_response *resp) {
    struct stat st;
    if (!root || !rel || stat(root, &st) != 0 || !S_ISDIR(st.st_mode) || !static_path_safe(rel)) return -1;
    sbuf path;
    sbuf_init(&path);
    sbuf_puts(&path, root);
    sbuf_puts(&path, rel);
    int is_dir = 0;
    size_t before = resp->body.len;
    int rc = load_file(path.data, &resp->body, &is_dir);
    if (rc == 0) {
        resp->status = 200;
        resp->content_type = static_mime_type(path.data);
        sbuf_puts(&resp->headers, "Cache-Control: no-cache\r\n");
    } else {
        resp->body.len = before;
    }
    sbuf_free(&path);
    return rc;
}
