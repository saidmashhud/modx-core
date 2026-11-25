/* Built against an installed modx-core (pkg-config or CMake find_package /
 * add_subdirectory; see tests/install_test.sh): the version, a store that
 * survives a reopen, a password checked on the worker pool and a listening
 * socket. Prints "ok <version>" and exits 0 when everything works. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "modx/crypto.h"
#include "modx/http.h"
#include "modx/pool.h"
#include "modx/store.h"
#include "modx/version.h"

static int verified = -1;
static void on_verify(void *ctx, int ok, int rehash) { (void)ctx; (void)rehash; verified = ok; }

int main(int argc, char **argv) {
    if (strcmp(modx_version(), MODX_VERSION) != 0) { fprintf(stderr, "header / library version mismatch\n"); return 1; }
    const char *dir = argc > 1 ? argv[1] : "./hello-data";
    static const mx_coll_def defs[] = { { "notes", 0, NULL } };
    char err[256];
    mx_store *st = mx_store_open(dir, defs, 1, 16, NULL, err, sizeof err);
    if (!st) { fprintf(stderr, "open: %s\n", err); return 1; }
    json *row = json_obj();
    json_set(row, "id", json_str("n1"));
    if (mx_store_put(st, 0, row, NULL) != 0 || mx_store_sync(st) != 0) return 1;
    int64_t seq = st->seq;
    mx_store_close(st);
    st = mx_store_open(dir, defs, 1, 16, NULL, err, sizeof err);
    if (!st || st->seq != seq || !mx_store_get(st, 0, "n1")) { fprintf(stderr, "reopen failed\n"); return 1; }
    mx_store_close(st);

    char *h = password_hash_iters("pw", 1000);
    mx_pool *pool = mx_pool_new(1, 0);
    if (!h || !pool || password_verify_async(pool, "pw", h, on_verify, NULL) != 0) return 1;
    while (verified < 0) mx_pool_wait(pool, 1000);
    mx_pool_free(pool);
    free(h);
    if (verified != 1) { fprintf(stderr, "verify failed\n"); return 1; }

    http_server srv;
    memset(&srv, 0, sizeof srv);
    srv.listen_fd = -1;
    if (http_server_listen(&srv, "127.0.0.1", 0) != 0 || srv.port <= 0) { perror("listen"); return 1; }
    http_server_close(&srv);
    printf("ok %s\n", modx_version());
    return 0;
}
