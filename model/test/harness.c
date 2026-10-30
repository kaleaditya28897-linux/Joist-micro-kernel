/* SPDX-License-Identifier: GPL-3.0-only */
/* harness.c - test runner for the host model. Runs every registered test and
 * exits non-zero if any check failed. An optional argument runs only the tests
 * whose name contains it. */
#include "harness.h"

#include <stdlib.h>
#include <string.h>

#define JT_MAX_TESTS 1024

static struct {
    const char *name;
    jt_fn fn;
} jt_tests[JT_MAX_TESTS];
static int jt_count;
static int jt_failed_checks;
static int jt_current_failed;

void jt_register(const char *name, jt_fn fn)
{
    if (jt_count >= JT_MAX_TESTS) {
        fprintf(stderr, "harness: more than %d tests\n", JT_MAX_TESTS);
        exit(2);
    }
    jt_tests[jt_count].name = name;
    jt_tests[jt_count].fn = fn;
    jt_count++;
}

void jt_fail(const char *file, int line, const char *expr, int fatal)
{
    fprintf(stderr, "  %s:%d: %s failed: %s\n", file, line, fatal ? "REQUIRE" : "CHECK", expr);
    jt_failed_checks++;
    jt_current_failed = 1;
}

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : NULL;
    int run = 0;
    int failed = 0;

    for (int i = 0; i < jt_count; i++) {
        if (filter != NULL && strstr(jt_tests[i].name, filter) == NULL) {
            continue;
        }
        jt_current_failed = 0;
        jt_tests[i].fn();
        run++;
        if (jt_current_failed) {
            failed++;
            fprintf(stderr, "FAIL %s\n", jt_tests[i].name);
        }
    }
    printf("%d tests, %d failed, %d failed checks\n", run, failed, jt_failed_checks);
    return failed == 0 ? 0 : 1;
}
