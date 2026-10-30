/* SPDX-License-Identifier: GPL-3.0-only */
/* fuzz_model.c - libFuzzer entry point (Phase 1 work item 5).
 *
 * The fuzz input is the stream of choices for the executor shared with the
 * whole-model property test (test/exec.c): one byte per choice of up to 256
 * values, two bytes for larger ones, so the fuzzer can steer every argument
 * and an 8 KiB input runs about a thousand steps. The run ends when the input does, then
 * tears down. Any violated invariant aborts, which libFuzzer records as a
 * crash with the input that caused it.
 *
 *   make fuzz FUZZ_TIME=600     run for ten minutes (corpus in build/fuzz-corpus) */
#include "exec.h"

#include <stdio.h>
#include <stdlib.h>

struct bytes {
    const uint8_t *p;
    size_t n;
};

static uint32_t bytes_next(void *ctx, uint32_t n)
{
    struct bytes *b = ctx;
    uint32_t v = 0;
    for (int i = 0; i < (n <= 256u ? 1 : 2) && b->n != 0; i++) {
        v = (v << 8) | *b->p++;
        b->n--;
    }
    return v % n;
}

static struct jx_stats st;

/* JOIST_FUZZ_STATS=1: print what the run reached, as test_props_all.c does,
 * so a fuzzer that never gets deep is visible. */
static void print_stats(void)
{
    for (unsigned op = 0; op < JX_OPS; op++) {
        fprintf(stderr, "  coverage %-13s", jx_op_name[op]);
        for (unsigned e = 0; e < JX_RESULTS; e++) {
            if (st.result[op][e] != 0) {
                fprintf(stderr, " %s=%lu", joist_err_name((joist_err)e), st.result[op][e]);
            }
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  %lu steps\n", st.steps);
}

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    const char *v = getenv("JOIST_FUZZ_STATS");
    if (v != NULL && *v == '1') {
        (void)atexit(print_stats);
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static struct joist_kernel k;
    char why[256];
    struct bytes b = {data, size};
    struct jx_source src = {bytes_next, &b};

    jx_begin(&k, &src);
    while (b.n != 0) {
        if (!jx_step(&k, &src, &st, why, sizeof why)) {
            fprintf(stderr, "fuzz_model: step %lu: %s\n", st.steps, why);
            abort();
        }
    }
    if (!jx_teardown(&k, &st, why, sizeof why)) {
        fprintf(stderr, "fuzz_model: teardown: %s\n", why);
        abort();
    }
    return 0;
}
