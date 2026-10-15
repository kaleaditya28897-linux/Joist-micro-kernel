/* test_smp.c - scheduling on more than one CPU (Phase 3 step 9; decision
 * 0016): who is on which CPU (I13), joist_sched_pick_on, joist_sched_better
 * and the resched flag. Unit tests, and a simulation of 2 and 4 CPUs under
 * one lock (one operation at a time, as the kernel runs them) checked
 * against two properties:
 *
 * - once every CPU that deserves it has chosen again, the threads on CPUs
 *   are the best that can run: no CPU would choose otherwise;
 * - when an operation leaves the resched flag clear, no CPU but the one
 *   acting needed to choose again (so the kernel, which only interrupts
 *   other CPUs when the flag is set, misses nothing). */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static struct joist_kernel k;
static joist_cspace root;

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

static void boot(void)
{
    joist_init(&k);
    (void)joist_boot(&k, 400, &root);
}

static joist_thread thread_with(joist_slot slot, uint8_t prio, uint64_t budget, uint64_t period)
{
    joist_thread t = 0;
    if (joist_thread_create(&k, root, &t) != JOIST_OK ||
        joist_sc_create(&k, root, slot, prio, budget, period) != JOIST_OK ||
        joist__sc_bind_thread(&k, k.cs[root].slot[slot].obj - 1u, t) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
    }
    return t;
}

static uint32_t pick(uint32_t cpu)
{
    joist_thread t;
    return joist_sched_pick_on(&k, cpu, &t) == JOIST_OK ? t : JOIST_MAX_THREADS;
}

JT_TEST(smp_each_cpu_gets_the_best_thread_no_other_has)
{
    boot();
    joist_thread a = thread_with(10, 9, 10, 10);
    joist_thread b = thread_with(11, 7, 10, 10);
    joist_thread c = thread_with(12, 5, 10, 10);
    JT_CHECK(pick(0) == a && pick(1) == b && pick(2) == c);
    JT_CHECK(k.th[a].cpu == 1 && k.th[b].cpu == 2 && k.on_cpu[2] == c + 1u);
    /* A CPU choosing again keeps its own, never takes another's, and with
     * nothing left is idle. */
    JT_CHECK(pick(1) == b && pick(3) == JOIST_MAX_THREADS && k.on_cpu[3] == 0);
    joist_thread t;
    JT_CHECK(joist_sched_pick_on(&k, 3, &t) == JOIST_E_EMPTY);
    JT_CHECK(joist_sched_pick_on(&k, JOIST_MAX_CPUS, &t) == JOIST_E_RANGE);
    /* The thread on CPU 0 blocks: CPU 0 has nothing better than nothing
     * (b and c are taken), until it chooses and finds none. */
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, root, 0, 1, JOIST_R_SEND, 1) == JOIST_OK);
    JT_REQUIRE(joist_wait(&k, a, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_sched_better(&k, 0) == 1);
    JT_CHECK(pick(0) == JOIST_MAX_THREADS && k.th[a].cpu == 0);
    JT_CHECK(joist_sched_better(&k, 0) == 0 && joist_sched_better(&k, 1) == 0);
    /* It wakes: an idle CPU deserves it, the busy ones don't (they run
     * lower priorities, but each CPU is asked about itself: CPU 2, on the
     * lowest, does). */
    k.resched = 0;
    JT_REQUIRE(joist_signal(&k, b, 1) == JOIST_OK);
    JT_CHECK(k.resched == 1);
    JT_CHECK(joist_sched_better(&k, 0) == 1 && joist_sched_better(&k, 2) == 1 &&
             joist_sched_better(&k, 1) == 1);
    JT_CHECK(pick(0) == a);
    JT_CHECK(joist_sched_better(&k, 1) == 0 && joist_sched_better(&k, 2) == 0);
    JT_CHECK(clean());
}

JT_TEST(smp_a_thread_touched_from_outside_leaves_its_cpu)
{
    boot();
    joist_thread a = thread_with(10, 9, 10, 10);
    joist_thread t;
    JT_REQUIRE(joist_task_create(&k, root, 20, 0) == JOIST_OK);
    JT_REQUIRE(joist_thread_spawn(&k, root, 0, 1, &t) == JOIST_OK);
    JT_REQUIRE(joist_sc_create(&k, root, 2, 5, 10, 10) == JOIST_OK &&
               joist_sc_bind(&k, root, 2, 1) == JOIST_OK);
    JT_CHECK(pick(0) == a && pick(1) == t && k.th[t].cpu == 2);
    /* Reading its registers, writing them, suspending it: each takes it off
     * its CPU first (the kernel's hook brings its registers home), and says
     * a CPU needs to choose again. */
    struct joist_regs regs;
    k.resched = 0;
    JT_CHECK(joist_thread_read_regs(&k, root, 1, &regs) == JOIST_OK);
    JT_CHECK(k.th[t].cpu == 0 && k.on_cpu[1] == 0 && k.resched == 1);
    JT_CHECK(pick(1) == t);
    JT_CHECK(joist_thread_write_regs(&k, root, 1, &regs) == JOIST_OK && k.th[t].cpu == 0);
    JT_CHECK(pick(1) == t);
    k.resched = 0;
    JT_CHECK(joist_thread_suspend(&k, root, 1) == JOIST_OK && k.th[t].cpu == 0 && k.resched == 1);
    JT_CHECK(pick(1) == JOIST_MAX_THREADS);
    k.resched = 0;
    JT_CHECK(joist_thread_resume(&k, root, 1) == JOIST_OK && k.resched == 1);
    JT_CHECK(pick(1) == t);
    /* Killed while on a CPU: the CPU is left with nothing, and I13 holds. */
    JT_CHECK(joist_thread_kill(&k, root, 1) == JOIST_OK && k.on_cpu[1] == 0);
    JT_CHECK(clean());
    /* Its context taken away while it runs: it stays where it is until its
     * CPU chooses again, which the flag asks for. */
    JT_REQUIRE(joist_thread_spawn(&k, root, 0, 3, &t) == JOIST_OK &&
               joist_sc_bind(&k, root, 2, 3) == JOIST_OK);
    JT_CHECK(pick(1) == t);
    k.resched = 0;
    JT_CHECK(joist_sc_unbind(&k, root, 2) == JOIST_OK && k.resched == 1);
    JT_CHECK(joist_sched_better(&k, 1) == 1 && pick(1) == JOIST_MAX_THREADS);
    JT_CHECK(clean());
}

static void off_cpu_hook(void *ctx, uint32_t t, uint32_t cpu)
{
    uint32_t *seen = ctx;
    seen[0]++;
    seen[1] = t;
    seen[2] = cpu;
}

JT_TEST(smp_the_kernel_is_told_before_a_running_thread_is_touched)
{
    uint32_t seen[3] = {0, 0, 0};
    boot();
    joist_thread t;
    JT_REQUIRE(joist_task_create(&k, root, 20, 0) == JOIST_OK);
    JT_REQUIRE(joist_thread_spawn(&k, root, 0, 1, &t) == JOIST_OK);
    JT_REQUIRE(joist_sc_create(&k, root, 2, 5, 10, 10) == JOIST_OK &&
               joist_sc_bind(&k, root, 2, 1) == JOIST_OK);
    k.hooks.off_cpu = off_cpu_hook;
    k.hooks.ctx = seen;
    /* Not on a CPU: nothing to tell. */
    JT_CHECK(joist_thread_suspend(&k, root, 1) == JOIST_OK && seen[0] == 0);
    JT_CHECK(joist_thread_resume(&k, root, 1) == JOIST_OK);
    JT_CHECK(pick(3) == t);
    JT_CHECK(joist_thread_suspend(&k, root, 1) == JOIST_OK);
    JT_CHECK(seen[0] == 1 && seen[1] == t && seen[2] == 3);
    JT_CHECK(joist_thread_resume(&k, root, 1) == JOIST_OK && pick(2) == t);
    JT_CHECK(joist_task_destroy(&k, root, 0) == JOIST_OK); /* its threads go first */
    JT_CHECK(seen[0] == 2 && seen[1] == t && seen[2] == 2);
    k.hooks = (struct joist_hooks){0};
    JT_CHECK(clean());
}

/* ---- the simulation ---- */

static uint64_t rng;
static uint32_t rnd(uint32_t n)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545F4914F6CDD1DULL) >> 33) % n;
}

/* (The small-table build has 8 threads in all.) */
#if JOIST_MAX_THREADS >= 16u
#define CLIENTS 10u
#define SERVERS 3u
#else
#define CLIENTS 4u
#define SERVERS 2u
#endif
static const struct joist_msg hello = {.r = {1, 2, 3, 4}};

/* Every CPU that deserves another thread chooses again, until none does. */
static unsigned settle(uint32_t ncpus)
{
    unsigned rounds = 0;
    for (int again = 1; again && rounds < 100u; rounds++) {
        again = 0;
        for (uint32_t c = 0; c < ncpus; c++) {
            if (joist_sched_better(&k, c)) {
                (void)pick(c);
                again = 1;
            }
        }
    }
    return rounds;
}

JT_TEST(props_smp_the_best_threads_run_and_no_wakeup_is_missed)
{
    unsigned long ops = 0, flagged = 0, quiet = 0, moved = 0, idle_cpus = 0, busy_all = 0;
    for (uint64_t seed = 1; seed <= 4000 && ops < 80000; seed++) {
        rng = seed * 0x9E3779B97F4A7C15ULL;
        uint32_t ncpus = seed % 2u ? 2u : 4u;
        boot();
        joist_thread server[SERVERS];
        joist_thread client[CLIENTS];
        JT_REQUIRE(joist_task_create(&k, root, 4u * CLIENTS, 30) == JOIST_OK);
        for (uint32_t i = 0; i < SERVERS; i++) {
            JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, i) == JOIST_OK);
            JT_REQUIRE(joist_thread_create(&k, root, &server[i]) == JOIST_OK);
            JT_REQUIRE(joist_recv(&k, server[i], i, JOIST_FOREVER) == JOIST_BLOCKED);
        }
        JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, 8) == JOIST_OK);
        JT_REQUIRE(joist_cap_mint(&k, root, 8, 9, JOIST_R_SEND, 1) == JOIST_OK);
        for (uint32_t i = 0; i < CLIENTS; i++) {
            /* Threads of a task, so that they can be suspended, resumed and
             * have their contexts taken and given back through capabilities
             * (thread in slot 40 + i, context in 50 + i). */
            uint64_t period = 20u + rnd(40);
            JT_REQUIRE(joist_thread_spawn(&k, root, 30, 40 + i, &client[i]) == JOIST_OK);
            JT_REQUIRE(joist_sc_create(&k, root, 50 + i, (uint8_t)(1u + rnd(3)),
                                       5u + rnd((uint32_t)period - 4u), period) == JOIST_OK);
            JT_REQUIRE(joist_sc_bind(&k, root, 50 + i, 40 + i) == JOIST_OK);
            /* They act through the root's capabilities: fixture-like. */
            k.th[client[i]].cs = root;
        }
        (void)settle(ncpus);
        for (int step = 0; step < 3000; step++) {
            /* Time: deadlines and periods come due (the timekeeper's tick). */
            k.resched = 0;
            uint64_t d = rnd(8) == 0 ? 1 : 0; /* (time moves slowly: budgets last) */
            k.now += d;
            for (uint32_t c = 0; c < ncpus; c++) {
                if (k.on_cpu[c] != 0) {
                    joist_sched_charge(&k, k.on_cpu[c] - 1u, d);
                }
            }
            joist_tick(&k, 0);
            (void)settle(ncpus); /* (a tick always asks every CPU) */
            /* One CPU's thread, or the root from outside, does one thing. */
            uint32_t cpu = rnd(ncpus);
            uint32_t t = k.on_cpu[cpu] != 0 ? k.on_cpu[cpu] - 1u : JOIST_MAX_THREADS;
            int before[JOIST_MAX_CPUS];
            for (uint32_t c = 0; c < ncpus; c++) {
                before[c] = joist_sched_better(&k, c);
            }
            k.resched = 0;
            uint32_t what = rnd(20);
            uint32_t victim = rnd(CLIENTS);
            if (what < 1) {
                (void)joist_thread_suspend(&k, root, 40 + victim);
            } else if (what < 3) {
                (void)joist_thread_resume(&k, root, 40 + victim);
            } else if (what < 4) {
                (void)joist_sc_unbind(&k, root, 50 + victim);
            } else if (what < 6) {
                (void)joist_sc_bind(&k, root, 50 + victim, 40 + victim);
            } else if (what < 8) {
                (void)joist_thread_abort(&k, root, 40 + victim);
            } else if (what < 10) {
                (void)joist_signal(&k, client[rnd(CLIENTS)], 9); /* (whoever: the model lets any running thread) */
            } else if (t == JOIST_MAX_THREADS) {
                /* an idle CPU does nothing */
            } else {
                struct joist_thread_rec *me = &k.th[t];
                uint32_t mine = SERVERS;
                for (uint32_t i = 0; i < SERVERS; i++) {
                    mine = server[i] == t ? i : mine;
                }
                if (me->reply_to != 0 && rnd(4) != 0) {
                    (void)joist_reply_recv(&k, t, &hello, mine < SERVERS ? mine : 0,
                                           rnd(8) == 0 ? 10 : JOIST_FOREVER);
                } else if (mine < SERVERS && me->reply_to == 0) {
                    (void)joist_recv(&k, t, mine, JOIST_FOREVER);
                } else if (rnd(12) == 0) {
                    (void)joist_wait(&k, t, 8, rnd(3) == 0 ? JOIST_FOREVER : 5u + rnd(20));
                } else if (rnd(3) == 0) {
                    (void)joist_call(&k, t, rnd(SERVERS), &hello, rnd(8) == 0 ? 15 : JOIST_FOREVER);
                }
            }
            ops++;
            /* The flag: clear means no other CPU's verdict turned to "choose
             * again" (the acting CPU chooses again whatever happens). */
            if (!k.resched) {
                quiet++;
                for (uint32_t c = 0; c < ncpus; c++) {
                    if (c != cpu && !before[c] && joist_sched_better(&k, c)) {
                        fprintf(stderr,
                                "  seed %" PRIu64 " step %d (%u CPUs): operation %u by thread %u on "
                                "CPU %u left the flag clear, but CPU %u now deserves another thread\n",
                                seed, step, ncpus, what, t, cpu, c);
                        JT_CHECK(0);
                        return;
                    }
                }
            } else {
                flagged++;
            }
            /* The acting CPU chooses; then whoever deserves to. */
            (void)pick(cpu);
            moved += settle(ncpus) > 1u;
            /* Settled: nobody would choose otherwise, so the threads on
             * CPUs are the best that can run. Said again directly: a thread
             * that can run and is on no CPU is no better than any thread on
             * one, and no CPU is idle while one waits. */
            char msg[200];
            if (joist_check_invariants(&k, msg, sizeof msg) != 0) {
                fprintf(stderr, "  seed %" PRIu64 " step %d: %s\n", seed, step, msg);
                JT_CHECK(0);
                return;
            }
            unsigned idle = 0;
            for (uint32_t c = 0; c < ncpus; c++) {
                idle += k.on_cpu[c] == 0;
            }
            for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
                const struct joist_thread_rec *w = &k.th[i];
                if (w->state != JOIST_T_RUNNING || w->run_sc == 0 || w->cpu != 0) {
                    continue;
                }
                const struct joist_object *wsc = &k.obj[w->run_sc - 1u];
                if (wsc->remaining == 0 && wsc->refill_at > k.now) {
                    continue; /* out of budget: can't run */
                }
                int ok = idle == 0;
                for (uint32_t c = 0; c < ncpus && ok; c++) {
                    const struct joist_thread_rec *r = &k.th[k.on_cpu[c] - 1u];
                    const struct joist_object *rsc = &k.obj[r->run_sc - 1u];
                    ok = rsc->prio > wsc->prio ||
                         (rsc->prio == wsc->prio && r->ready_seq < w->ready_seq);
                }
                if (!ok) {
                    fprintf(stderr,
                            "  seed %" PRIu64 " step %d (%u CPUs): thread %u waits though a CPU is "
                            "idle or runs something no better\n",
                            seed, step, ncpus, i);
                    JT_CHECK(0);
                    return;
                }
            }
            idle_cpus += idle != 0;
            busy_all += idle == 0;
        }
    }
    fprintf(stderr,
            "  coverage smp: %lu operations (%lu set the flag, %lu left it clear); settling took "
            "more than one round %lu times; some CPU idle after %lu, all busy after %lu\n",
            ops, flagged, quiet, moved, idle_cpus, busy_all);
    JT_CHECK(flagged > 5000 && quiet > 5000 && idle_cpus > 5000 && (busy_all > 5000 || CLIENTS < 10u));
}
