/* test_props_all.c - the whole-model property test (Phase 1 work item 5).
 *
 * Every operation of every area, interleaved, from a seeded RNG, through the
 * executor shared with the fuzzer (exec.c). After every step: every structural
 * invariant (joist_check_invariants), I8 directly, and I9 as a work bound. At
 * the end of each sequence: teardown must leave nothing (I1, I3, I10).
 *
 * The per-area property tests compare against shadow models and so check
 * results; this one has no shadow and checks invariants only, but reaches
 * interleavings the per-area tests can't (IPC across tasks being destroyed,
 * no-senders firing during a cascade, thread objects killed mid-call).
 *
 *   JOIST_PROP_SEEDS=N   number of seeds to run (default 200)
 *   JOIST_PROP_STEPS=N   steps per seed (default 2000)
 *   JOIST_PROP_SEED=S    run only seed S (replay a reported failure) */
#include "exec.h"
#include "harness.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state;

static uint32_t rng_next(void *ctx, uint32_t n)
{
    (void)ctx;
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1DULL) % n);
}

static unsigned long env_ulong(const char *name, unsigned long def)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? strtoul(v, NULL, 0) : def;
}

static struct joist_kernel k;
static struct jx_stats st;

static int run_seed(uint64_t seed, unsigned long steps)
{
    char why[256];
    struct jx_source src = {rng_next, NULL};
    rng_state = seed * 0x9E3779B97F4A7C15ULL + 1u;
    jx_begin(&k, &src);
    for (unsigned long step = 0; step < steps; step++) {
        if (!jx_step(&k, &src, &st, why, sizeof why)) {
            fprintf(stderr, "  seed %" PRIu64 " step %lu: %s\n", seed, step, why);
            return 0;
        }
    }
    if (!jx_teardown(&k, &st, why, sizeof why)) {
        fprintf(stderr, "  seed %" PRIu64 " teardown: %s\n", seed, why);
        return 0;
    }
    st.sequences++;
    return 1;
}

JT_TEST(props_all_invariants_hold)
{
    unsigned long steps = env_ulong("JOIST_PROP_STEPS", 2000);
    const char *one = getenv("JOIST_PROP_SEED");
    memset(&st, 0, sizeof st);

    if (one != NULL && *one != '\0') {
        JT_CHECK(run_seed(strtoull(one, NULL, 0), steps));
    } else {
        unsigned long seeds = env_ulong("JOIST_PROP_SEEDS", 200);
        for (unsigned long s = 1; s <= seeds; s++) {
            if (!run_seed(s, steps)) {
                JT_CHECK(0 && "property failed; replay with JOIST_PROP_SEED");
                break;
            }
        }
    }
    for (unsigned op = 0; op < JX_OPS; op++) {
        fprintf(stderr, "  coverage %-13s", jx_op_name[op]);
        for (unsigned e = 0; e < JX_RESULTS; e++) {
            if (st.result[op][e] != 0) {
                fprintf(stderr, " %s=%lu", joist_err_name((joist_err)e), st.result[op][e]);
            }
        }
        fprintf(stderr, "  [max work %" PRIu64 " of %" PRIu64 "]\n", st.max_work[op],
                jx_op_cascades(op) ? JX_WORK_CASCADE : JX_WORK_SIMPLE);
    }
    fprintf(stderr, "  coverage wake-ups     ");
    for (unsigned e = 0; e < JX_RESULTS; e++) {
        if (st.wakeups[e] != 0) {
            fprintf(stderr, " %s=%lu", joist_err_name((joist_err)e), st.wakeups[e]);
        }
    }
    fprintf(stderr,
            "\n  %lu sequences, %lu steps, every teardown clean; %lu tasks in cycles reaped "
            "by the root\n",
            st.sequences, st.steps, st.cycles);
    fprintf(stderr, "  coverage transfer      %lu capabilities arrived, %lu skipped\n",
            st.caps_moved, st.caps_skipped);
    fprintf(stderr, "  coverage donation      %lu steps with a context lent, %lu with a chain\n",
            st.lent, st.chains);
    fprintf(stderr, "  coverage faults        %lu to a handler, %lu with none (task or thread ended)\n",
            st.faults_taken, st.faults_fatal);
    fprintf(stderr, "  coverage memory        %lu steps with a mapping standing, %lu with page tables, "
                    "%lu with a frame dying while mapped\n",
            st.mapped, st.tables, st.frames_dying);
    fprintf(stderr, "  coverage preemption    %lu steps with a destruction half done, %lu with two or more\n",
            st.dying, st.dying_deep);
    if (one == NULL || *one == '\0') {
        /* The full run must actually move capabilities and skip some. */
        JT_CHECK(st.caps_moved > 100 && st.caps_skipped > 100);
        JT_CHECK(st.lent > 100 && st.chains > 10);
        JT_CHECK(st.faults_taken > 100 && st.faults_fatal > 100);
        JT_CHECK(st.mapped > 1000 && st.tables > 1000 && st.frames_dying > 100);
        JT_CHECK(st.result[JX_MAP][0] > 200 && st.result[JX_UNMAP][0] > 200 &&
                 st.result[JX_PROTECT][0] > 200 && st.result[JX_DONATE][0] > 50 &&
                 st.result[JX_MAP_QUERY][0] > 200);
        JT_CHECK(st.result[JX_REGION][0] > 500); /* regions made and used (0030) */
        JT_CHECK(st.dying > 1000 && st.dying_deep > 10);
    }
}
