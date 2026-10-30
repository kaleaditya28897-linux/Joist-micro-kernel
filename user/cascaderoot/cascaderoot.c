/* SPDX-License-Identifier: GPL-3.0-only */
/* cascaderoot.c - a root task that tests preemptible destruction from user
 * mode (Phase 3 step 7; decision 0016; docs/abi.md, *Task destruction*).
 *
 * It builds a chain of tasks, each with a capability table holding 100
 * endpoints of its own and the only capability to the next, and destroys
 * the first: the whole chain goes, a cascade, which is the longest thing
 * the kernel ever does in one go. Two things must hold.
 *
 * 1. The kernel stays responsive meanwhile. A thread of higher priority
 *    (module 0, worker, as a ticker) sleeps a fixed time again and again
 *    and records the longest it ever waited between two wake-ups; the root
 *    times each cascade. The ticker's period is set to a tenth of what a
 *    cascade takes, so a kernel that does a cascade in one piece makes the
 *    ticker wait about ten periods, and one that preempts, about one.
 *    Times are the time stamp counter's, read here and in the ticker.
 *
 * 2. Everything comes back. The rounds are repeated, and the number of
 *    tasks the root can create is the same after them as before. */
#define TEST_NAME "cascade"
#include "../lib/testroot.h"

/* Sized for the kernel the self-test builds for it, with larger tables
 * than the default (kernel/test/selftest.sh): with the default ones a whole
 * cascade is too short to measure against. */
#define TASKS  48u  /* in the chain */
#define CAPS   100u /* in each one's table, besides the next task: an endpoint each */
#define NEXT   127u /* the slot that holds the next task */
#define ROUNDS 20u

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus);

static uint64_t chain[TASKS];
static uint64_t ep;

/* The chain: the root keeps a capability to the first task only. */
static void build(void)
{
    for (unsigned i = 0; i < TASKS; i++) {
        must(j_task_create(4, chain[i]), "task_create");
        for (unsigned c = 0; c < CAPS; c++) {
            /* An endpoint of its own in every slot, held nowhere else: each
             * slot swept destroys an object. */
            must(j_obj_create(J_ENDPOINT, ep), "obj_create");
            must(j_cap_give(chain[i], ep, c, J_R_ALL), "cap_give");
            must(j_cap_delete(ep), "cap_delete");
        }
    }
    for (unsigned i = TASKS - 1u; i >= 1u; i--) {
        must(j_cap_give(chain[i - 1u], chain[i], NEXT, J_R_ALL), "cap_give (the next task)");
        must(j_cap_delete(chain[i]), "cap_delete");
    }
}

/* Destroy it and see it through; the time it took (TSC). */
static uint64_t destroy(void)
{
    int more = 1;
    uint64_t t0 = work_tsc();
    must(j_task_destroy(chain[0]), "task_destroy");
    while (more) {
        must(j_continue(0, &more), "continue");
    }
    uint64_t t1 = work_tsc();
    must(j_cap_delete(chain[0]), "cap_delete (the first task)");
    return t1 - t0;
}

/* How many tasks the root can create right now (they are destroyed again). */
static unsigned capacity(void)
{
    static uint64_t slot[64];
    unsigned n = 0;
    int more = 1;
    while (n < 64u) {
        if (slot[n] == 0) {
            slot[n] = slot_new();
        }
        if (j_task_create(4, slot[n]) != J_OK) {
            break;
        }
        n++;
    }
    for (unsigned i = 0; i < n; i++) {
        must(j_task_destroy(slot[i]), "task_destroy");
        must(j_cap_delete(slot[i]), "cap_delete");
    }
    while (more) {
        must(j_continue(0, &more), "continue");
    }
    return n;
}

static void put(const char *what, uint64_t v)
{
    j_puts(what);
    j_putu(v);
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 1) {
        die("needs one module: worker");
    }
    test_init();
    occupy_other_cpus(ncpus); /* written for one CPU: lib/testroot.h */
    ep = slot_new();
    for (unsigned i = 0; i < TASKS; i++) {
        chain[i] = slot_new();
    }

    /* The clock: TSC counts in a millisecond. */
    uint64_t t0 = work_tsc();
    sleep_ns(50 * J_MS);
    uint64_t per_ms = (work_tsc() - t0) / 50u;
    check(per_ms != 0, "the time stamp counter doesn't advance");

    /* How long a cascade takes, with nobody else about (the shortest of 3). */
    uint64_t alone = UINT64_MAX;
    for (unsigned i = 0; i < 3u; i++) {
        build();
        uint64_t d = destroy();
        alone = d < alone ? d : alone;
    }
    /* The ticker: a tenth of that, and not below 20 us. */
    uint64_t period_ns = alone * 100000u / per_ms; /* alone / 10, in ns */
    if (period_ns < 20000u) {
        period_ns = 20000u;
    }
    uint64_t period = period_ns * per_ms / 1000000u; /* in TSC counts */
    check(alone >= 5u * period, "a cascade is too short to tell preempted from not");

    struct worker tick = worker_new(WORK_TICK, period_ns);
    uint64_t tep = slot_new();
    must(j_obj_create(J_ENDPOINT, tep), "obj_create");
    must(j_cap_give(tick.task, tep, 0, J_R_RECV), "cap_give (ticker)");
    worker_run(&tick, 200, 10 * J_MS, 10 * J_MS);

    unsigned before = capacity();
    check(before >= TASKS, "the root can't create enough tasks for the chain");

    /* What the ticker's longest wait is with the root asleep. */
    sleep_ns(20 * J_MS);
    tick.w->aux = 0;
    sleep_ns(50 * J_MS);
    uint64_t quiet = tick.w->aux;

    /* The rounds: how long each cascade took, and the ticker's longest
     * wait during it. */
    static uint64_t took[ROUNDS];
    static uint64_t waited[ROUNDS];
    uint64_t ticks = 0;
    for (unsigned r = 0; r < ROUNDS; r++) {
        build();
        tick.w->aux = 0;
        uint64_t c0 = tick.w->count;
        took[r] = destroy();
        ticks += tick.w->count - c0;
        waited[r] = tick.w->aux;
    }
    /* A round in which the machine itself stalled (a virtual CPU that lost
     * its host CPU for a while) shows it twice: the ticker waited, and the
     * cascade took that much longer too. A kernel that holds the ticker up
     * doesn't make the cascade any longer. So rounds that took over one and
     * a half times the median are set aside, a few at most, and the rest
     * must all be good. */
    static uint64_t sorted[ROUNDS];
    for (unsigned r = 0; r < ROUNDS; r++) {
        unsigned k = r;
        for (; k > 0 && sorted[k - 1u] > took[r]; k--) {
            sorted[k] = sorted[k - 1u];
        }
        sorted[k] = took[r];
    }
    uint64_t median = sorted[ROUNDS / 2u];
    uint64_t worst = 0;
    unsigned aside = 0;
    for (unsigned r = 0; r < ROUNDS; r++) {
        if (took[r] > median + median / 2u) {
            aside++;
        } else if (waited[r] > worst) {
            worst = waited[r];
        }
    }

    put("cascade: ", ROUNDS);
    put(" cascades of ", TASKS);
    put(" tasks, ", (uint64_t)TASKS * (CAPS + 1u));
    put(" capabilities; alone one takes ", alone * 1000u / per_ms);
    put(" us, beside the ticker ", median * 1000u / per_ms);
    put(" us (median; ", aside);
    j_puts(" rounds set aside as stalled)\n");
    put("cascade: ticker every ", period_ns / 1000u);
    put(" us: woke ", ticks);
    put(" times during them; its longest wait ", worst * 1000u / per_ms);
    put(" us (", quiet * 1000u / per_ms);
    j_puts(" us with the root asleep)\n");

    check(aside <= ROUNDS / 5u, "the machine stalled too often to measure");
    check(worst < alone / 2u, "a higher priority waited for a whole cascade");
    check(ticks >= ROUNDS * 3u, "the ticker hardly ran while tasks were being destroyed");
    j_puts("cascade: preemption ok\n");

    unsigned after = capacity();
    worker_stop(&tick);
    put("cascade: tasks the root can create: ", before);
    put(" before, ", after);
    j_puts(" after\n");
    check(after == before, "something didn't come back");
    j_puts("cascade: all ok\n");
    j_exit(0);
}
