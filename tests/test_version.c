/* Version: header macros agree with each other, with the library and with CHANGELOG.md. */
#include <stdio.h>
#include <stdlib.h>

#include "modx/version.h"
#include "test.h"

static void t_version(void) {
    char want[32];
    snprintf(want, sizeof want, "%d.%d.%d", MODX_VERSION_MAJOR, MODX_VERSION_MINOR, MODX_VERSION_PATCH);
    CHECK_STR(MODX_VERSION, want);
    CHECK_STR(modx_version(), MODX_VERSION);
    CHECK(MODX_VERSION_NUM == MODX_VERSION_MAJOR * 10000 + MODX_VERSION_MINOR * 100 + MODX_VERSION_PATCH);
    /* the newest CHANGELOG section is this version (skipped when run outside the repository) */
    FILE *f = fopen("CHANGELOG.md", "r");
    if (!f) return;
    char line[256], head[64];
    snprintf(head, sizeof head, "## %s", MODX_VERSION);
    int first = 1, ok = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "## ", 3) != 0) continue;
        if (first) ok = strncmp(line, head, strlen(head)) == 0 && (line[strlen(head)] == ' ' || line[strlen(head)] == '\n');
        first = 0;
    }
    fclose(f);
    CHECK(ok);
}

void test_version_all(void) { RUN(t_version); }
