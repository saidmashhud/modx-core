#ifndef MODX_STATIC_H
#define MODX_STATIC_H

#include "modx/http.h"

const char *static_mime_type(const char *path);
/* Serve a file from root for GET/HEAD (SPA fallback to index.html). */
void static_serve(const char *root, const http_request *req, http_response *resp);
/* Returns 1 if the decoded URL path is safe to map onto the filesystem. */
int static_path_safe(const char *path);
/* Serve exactly root + rel (no SPA fallback, Cache-Control: no-cache): 0, or
 * -1 when the file does not exist (resp untouched). E.g. a generated API document. */
int static_serve_exact(const char *root, const char *rel, http_response *resp);

#endif
