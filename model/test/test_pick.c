/* test_pick.c - the scheduler's rules (Phase 3 step 5; §5.6): who runs,
 * budgets and periods, and the next timer event. Unit tests, and a
 * simulation checked against an independent reading of the rules. */
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
    (void)joist_boot(&k, 200, &root);
}

/* A fixture thread in the root with a context of its own in root slot
 * `slot`. */
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

static uint32_t picked(void)
{
    joist_thread t;
    return joist_sched_pick(&k, &t) == JOIST_OK ? t : JOIST_MAX_THREADS;
}

JT_TEST(pick_highest_priority_that_can_run)
{
    boot();
    joist_thread lo = thread_with(10, 5, 10, 10);
    joist_thread hi = thread_with(11, 9, 10, 10);
    joist_thread passive;
    JT_REQUIRE(joist_thread_create(&k, root, &passive) == JOIST_OK);
    JT_CHECK(picked() == hi);
    /* A passive thread never runs, whatever else does or doesn't. */
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, 0) == JOIST_OK);
    JT_REQUIRE(joist_wait(&k, hi, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(picked() == lo);
    JT_REQUIRE(joist_wait(&k, lo, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    joist_thread t;
    JT_CHECK(joist_sched_pick(&k, &t) == JOIST_E_EMPTY);
    JT_CHECK(k.th[passive].state == JOIST_T_RUNNING); /* ready, but with nothing to run on */
    JT_CHECK(clean());
}

JT_TEST(pick_equal_priorities_take_turns)
{
    boot();
    joist_thread a = thread_with(10, 5, 2, 2);
    joist_thread b = thread_with(11, 5, 2, 2);
    joist_thread c = thread_with(12, 5, 2, 2);
    /* Created in this order, so they run in it, each for its budget. */
    uint32_t order[6];
    for (unsigned i = 0; i < 6u; i++) {
        order[i] = picked();
        joist_sched_charge(&k, order[i], 2);
        joist_tick(&k, 2);
    }
    JT_CHECK(order[0] == a && order[1] == b && order[2] == c);
    JT_CHECK(order[3] == a && order[4] == b && order[5] == c);
    /* A partial use keeps the turn. */
    uint32_t t = picked();
    joist_sched_charge(&k, t, 1);
    joist_tick(&k, 1);
    JT_CHECK(picked() == t);
    JT_CHECK(clean());
}

JT_TEST(pick_budget_is_enforced_per_period)
{
    boot();
    joist_thread hog = thread_with(10, 9, 2, 10); /* high priority, 20% */
    joist_thread low = thread_with(11, 1, 100, 100);
    JT_CHECK(picked() == hog);
    JT_CHECK(joist_sched_next_event(&k, hog) == 2); /* its budget runs out */
    joist_sched_charge(&k, hog, 2);
    joist_tick(&k, 2);
    JT_CHECK(picked() == low);                      /* out of budget until time 10 */
    JT_CHECK(joist_sched_next_event(&k, low) == 10); /* the hog's next period */
    joist_sched_charge(&k, low, 8);
    joist_tick(&k, 8);
    JT_CHECK(k.now == 10 && picked() == hog); /* a new period: whole budget */
    JT_CHECK(k.obj[k.cs[root].slot[10].obj - 1u].remaining == 2);
    JT_CHECK(clean());
}

JT_TEST(pick_server_runs_on_the_clients_budget)
{
    /* Donation, where it matters: a passive server is picked because its
     * client's context is, and what it uses is the client's. */
    boot();
    joist_thread c = thread_with(10, 7, 5, 10);
    joist_thread s;
    JT_REQUIRE(joist_thread_create(&k, root, &s) == JOIST_OK);
    joist_thread other = thread_with(11, 3, 10, 10);
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, s, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, 0, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(picked() == s); /* at the client's priority 7, above `other` */
    joist_sched_charge(&k, s, 3);
    joist_tick(&k, 3);
    JT_CHECK(k.obj[k.cs[root].slot[10].obj - 1u].remaining == 2); /* the client paid */
    JT_REQUIRE(joist_reply(&k, s, &m) == JOIST_OK);
    JT_CHECK(picked() == c); /* back with its context, and what is left of it */
    joist_sched_charge(&k, c, 2);
    joist_tick(&k, 2);
    JT_CHECK(picked() == other); /* the client's budget is gone, server included */
    JT_CHECK(clean());
}

JT_TEST(pick_next_event)
{
    boot();
    joist_thread t;
    JT_CHECK(joist_sched_next_event(&k, JOIST_MAX_THREADS) == JOIST_FOREVER);
    joist_thread a = thread_with(10, 5, 4, 20);
    joist_thread b = thread_with(11, 4, 50, 50);
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, 0) == JOIST_OK);
    JT_REQUIRE(joist_sched_pick(&k, &t) == JOIST_OK && t == a);
    JT_CHECK(joist_sched_next_event(&k, a) == 4);
    /* A blocked thread's deadline comes first if it is sooner. */
    JT_REQUIRE(joist_wait(&k, b, 0, 3) == JOIST_BLOCKED);
    JT_CHECK(joist_sched_next_event(&k, a) == 3);
    joist_sched_charge(&k, a, 3);
    joist_tick(&k, 3); /* b times out and is ready again */
    JT_CHECK(k.th[b].state == JOIST_T_RUNNING && picked() == a);
    joist_sched_charge(&k, a, 1);
    joist_tick(&k, 1);
    JT_CHECK(picked() == b);
    /* a waits for its next period, at 20. */
    JT_CHECK(joist_sched_next_event(&k, b) == 20);
    JT_CHECK(clean());
}

JT_TEST(pick_woken_thread_goes_behind_its_equals)
{
    boot();
    joist_thread a = thread_with(10, 5, 10, 10);
    joist_thread b = thread_with(11, 5, 10, 10);
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, root, 0, 1, JOIST_R_SEND, 1) == JOIST_OK);
    JT_REQUIRE(picked() == a);
    JT_REQUIRE(joist_wait(&k, a, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(picked() == b);
    JT_REQUIRE(joist_signal(&k, b, 1) == JOIST_OK); /* a is ready again */
    JT_CHECK(picked() == b);                        /* but b keeps its turn */
    joist_sched_charge(&k, b, 10);
    joist_tick(&k, 10);
    JT_CHECK(picked() == a);
    JT_CHECK(clean());
}

/* ---- simulation ---- */

#define NT 6u

JT_TEST(props_pick_simulation)
{
    unsigned long decisions = 0, idle = 0, refills = 0;
    for (uint64_t seed = 1; seed <= 60; seed++) {
        boot();
        srand((unsigned)seed);
        joist_thread th[NT];
        uint32_t sc[NT];        /* its context object */
        uint64_t used[NT];      /* consumed in the period that began at start[i] */
        uint64_t start[NT];
        uint8_t prio[NT];
        uint64_t budget[NT];
        uint64_t period[NT];
        for (uint32_t i = 0; i < NT; i++) {
            prio[i] = (uint8_t)(rand() % 4);
            period[i] = 4u + (uint64_t)(rand() % 20);
            /* Odd seeds: light load (each at most a quarter of its period),
             * so the system is often idle, waiting for a period to start;
             * even seeds: usually overcommitted. */
            budget[i] = 1u + (uint64_t)rand() % (seed % 2u != 0 ? period[i] / 4u + 1u : period[i]);
            th[i] = thread_with(10u + i, prio[i], budget[i], period[i]);
            sc[i] = k.cs[root].slot[10u + i].obj - 1u;
            used[i] = 0;
            start[i] = 0;
        }
        for (int step = 0; step < 400; step++) {
            joist_thread t;
            joist_err e = joist_sched_pick(&k, &t);
            /* The rules, read independently: who can run, and who is best. */
            uint32_t want = JOIST_MAX_THREADS;
            for (uint32_t i = 0; i < NT; i++) {
                const struct joist_object *o = &k.obj[sc[i]];
                int can = k.th[th[i]].state == JOIST_T_RUNNING &&
                          (o->remaining != 0 || o->refill_at <= k.now);
                /* (pick already refilled the winner; a due refill counts.) */
                if (!can) {
                    continue;
                }
                if (want == JOIST_MAX_THREADS || prio[i] > prio[want] ||
                    (prio[i] == prio[want] &&
                     k.th[th[i]].ready_seq < k.th[th[want]].ready_seq)) {
                    want = i;
                }
            }
            if ((e == JOIST_OK) != (want != JOIST_MAX_THREADS) ||
                (e == JOIST_OK && t != th[want])) {
                fprintf(stderr, "  seed %" PRIu64 " step %d: picked %u, the rules say %u\n", seed,
                        step, e == JOIST_OK ? t : 999u,
                        want != JOIST_MAX_THREADS ? th[want] : 999u);
                JT_CHECK(0);
                return;
            }
            uint64_t next = joist_sched_next_event(&k, e == JOIST_OK ? t : JOIST_MAX_THREADS);
            if (next == JOIST_FOREVER || next <= k.now) {
                fprintf(stderr, "  seed %" PRIu64 " step %d: next event %" PRIu64 " at %" PRIu64 "\n",
                        seed, step, next, k.now);
                JT_CHECK(0); /* with periodic contexts something always comes, later */
                return;
            }
            uint64_t dt = next - k.now;
            if (rand() % 3 == 0 && dt > 1) {
                dt = 1u + (uint64_t)rand() % (dt - 1u); /* an interrupt or a call, sooner */
            }
            if (e == JOIST_OK) {
                decisions++;
                /* A new period for the winner began at this pick if its
                 * record says so. */
                const struct joist_object *o = &k.obj[sc[want]];
                if (o->refill_at == k.now + period[want] && start[want] != k.now) {
                    start[want] = k.now;
                    used[want] = 0;
                    refills++;
                }
                used[want] += dt;
                if (used[want] > budget[want]) {
                    fprintf(stderr, "  seed %" PRIu64 " step %d: thread %u used %" PRIu64
                                    " of a budget of %" PRIu64 " in one period\n",
                            seed, step, th[want], used[want], budget[want]);
                    JT_CHECK(0);
                    return;
                }
                joist_sched_charge(&k, t, dt);
                /* Now and then it blocks for a while instead of running on. */
                if (rand() % 8 == 0 && k.cs[root].slot[0].obj == 0) {
                    (void)joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, 0);
                }
                if (rand() % 8 == 0 && k.cs[root].slot[0].obj != 0) {
                    (void)joist_wait(&k, t, 0, 1u + (uint64_t)(rand() % 30));
                }
            } else {
                idle++;
            }
            joist_tick(&k, dt);
            if (!clean()) {
                JT_CHECK(0);
                return;
            }
        }
    }
    fprintf(stderr, "  coverage pick decisions=%lu idle=%lu new periods=%lu\n", decisions, idle,
            refills);
    JT_CHECK(decisions > 5000 && idle > 100 && refills > 1000);
}
