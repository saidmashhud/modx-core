#ifndef MODX_STATIC_H
#define MODX_STATIC_H

/**
 * @file static.h
 * @brief Static file serving into an http_response (whole file read into the body),
 * with MIME types, weak ETags / 304 and an SPA fallback.
 *
 * Not thread-safe (keeps the last stat in a static); call on the loop thread.
 */

#include "modx/http.h"

/** MIME type for a path's extension (case-insensitive); "application/octet-stream" when unknown. */
const char *static_mime_type(const char *path);
/**
 * Serve a file from root for GET/HEAD (the method is not checked).
 *
 * Maps req->path under @p root ("/" and directories -> index.html). Unknown
 * paths get the app shell (SPA fallback): root/app.html if present, else
 * root/index.html, with Cache-Control: no-cache; missing files under /assets/
 * stay 404. Files under /assets/ are sent as immutable for a year, others
 * no-cache. Adds a weak ETag and answers 304 on a matching If-None-Match.
 * Unsafe paths get 400, a missing root or file 404.
 */
void static_serve(const char *root, const http_request *req, http_response *resp);
/** Returns 1 if the decoded URL path is safe to map onto the filesystem
 * (starts with '/', no '\\', no "." or ".." segments). */
int static_path_safe(const char *path);
/** Serve exactly root + rel (no SPA fallback, no ETag, Cache-Control: no-cache): 0, or
 * -1 when the file does not exist or rel is unsafe (resp untouched). E.g. a generated API document. */
int static_serve_exact(const char *root, const char *rel, http_response *resp);

#endif
