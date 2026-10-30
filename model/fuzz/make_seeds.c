/* SPDX-License-Identifier: GPL-3.0-only */
/* make_seeds.c - write the fuzzer's seed corpus (Phase 1 work item 5).
 *
 * Runs the executor from the same seeded RNG as test_props_all.c and records
 * every choice in the fuzz input encoding (fuzz_model.c: one byte for a
 * choice of up to 256 values, two bytes big-endian otherwise), so each file
 * replays one property-test sequence exactly under the fuzzer. libFuzzer then
 * mutates inputs that already reach deep states, instead of starting from
 * bytes that mostly decode to the first few operations.
 *
 *   make-seeds DIR SEEDS STEPS */
#include "exec.h"

#include <stdio.h>
#include <stdlib.h>

struct rec {
    uint64_t state;
    FILE *out;
};

static uint32_t rec_next(void *ctx, uint32_t n)
{
    struct rec *r = ctx;
    r->state ^= r->state >> 12;
    r->state ^= r->state << 25;
    r->state ^= r->state >> 27;
    uint32_t v = (uint32_t)((r->state * 0x2545F4914F6CDD1DULL) % n);
    if (n > 256u) {
        (void)fputc((int)(v >> 8), r->out);
    }
    (void)fputc((int)(v & 0xffu), r->out);
    return v;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: make-seeds DIR SEEDS STEPS\n");
        return 2;
    }
    unsigned long seeds = strtoul(argv[2], NULL, 0);
    unsigned long steps = strtoul(argv[3], NULL, 0);
    static struct joist_kernel k;
    static struct jx_stats st;
    char why[256];
    char path[4096];
    for (unsigned long s = 1; s <= seeds; s++) {
        (void)snprintf(path, sizeof path, "%s/seed-%lu", argv[1], s);
        struct rec r = {s * 0x9E3779B97F4A7C15ULL + 1u, fopen(path, "wb")};
        if (r.out == NULL) {
            perror(path);
            return 1;
        }
        struct jx_source src = {rec_next, &r};
        jx_begin(&k, &src);
        for (unsigned long i = 0; i < steps; i++) {
            if (!jx_step(&k, &src, &st, why, sizeof why)) {
                fprintf(stderr, "make-seeds: seed %lu step %lu: %s\n", s, i, why);
                return 1;
            }
        }
        (void)fclose(r.out);
    }
    return 0;
}
