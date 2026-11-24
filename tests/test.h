#ifndef MODX_TEST_H
#define MODX_TEST_H

#include <stdio.h>
#include <string.h>

extern int t_checks, t_failures;

#define CHECK(cond) do { \
    t_checks++; \
    if (!(cond)) { t_failures++; fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_STR(a, b) do { \
    const char *a_ = (a), *b_ = (b); \
    t_checks++; \
    if (!a_ || !b_ || strcmp(a_, b_) != 0) { \
        t_failures++; \
        fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
    } \
} while (0)

#define RUN(fn) do { fprintf(stderr, "- %s\n", #fn); fn(); } while (0)

void test_json_all(void);
void test_crypto_all(void);
void test_session_all(void);
void test_ot_all(void);
void test_http_all(void);
void test_store_all(void);
void test_timer_all(void);
void test_ws_all(void);
void test_gzip_all(void);
void test_idem_all(void);
void test_version_all(void);

#endif
