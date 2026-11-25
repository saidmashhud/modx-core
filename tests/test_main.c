#include "test.h"

int t_checks, t_failures;

int main(void) {
    test_json_all();
    test_crypto_all();
    test_session_all();
    test_ot_all();
    test_http_all();
    test_store_all();
    test_timer_all();
    test_gzip_all();
    test_ws_all();
    test_idem_all();
    test_version_all();
    test_pool_all();
    test_net_all();
    fprintf(stderr, "\nunit: %d checks, %d failed\n", t_checks, t_failures);
    return t_failures ? 1 : 0;
}
