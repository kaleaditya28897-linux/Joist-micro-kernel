/* test_runq.c - the run queues (Phase 3 step 10; choice 9): a thread is on
 * a queue exactly when it can run and is on no CPU (I14), the queues choose
 * what the scan would, and a choice costs the same however many threads
 * there are. Every other test checks I14 too, through the invariants, and
 * with it the queues' choice against the scan for every CPU. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

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

static uint32_t picked_on(uint32_t cpu)
{
    joist_thread t;
    return joist_sched_pick_on(&k, cpu, &t) == JOIST_OK ? t : JOIST_MAX_THREADS;
}

static const struct joist_msg hello = {.r = {1, 2, 3, 4}};

JT_TEST(runq_queued_exactly_when_ready)
{
    boot();
    joist_thread a = thread_with(10, 5, 10, 100);
    joist_thread b = thread_with(11, 5, 10, 100);
    joist_thread lo = thread_with(12, 3, 10, 100);
    joist_thread passive;
    JT_REQUIRE(joist_thread_create(&k, root, &passive) == JOIST_OK);
    JT_CHECK(k.th[a].rq_on != 0 && k.th[b].rq_on != 0 && k.th[lo].rq_on != 0);
    JT_CHECK(k.th[passive].rq_on == 0); /* no context: never queued */
    JT_CHECK(picked_on(0) == a && k.th[a].rq_on == 0); /* on a CPU: off the queues */
    /* Waiting takes it off; a signal puts it back. */
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, root, 0, 1, JOIST_R_SEND, 1) == JOIST_OK);
    JT_REQUIRE(joist_wait(&k, b, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[b].rq_on == 0 && clean());
    JT_REQUIRE(joist_signal(&k, a, 1) == JOIST_OK);
    JT_CHECK(k.th[b].rq_on != 0 && clean());
    /* Spending its budget takes it off until a tick at its refill. */
    joist_sched_charge(&k, a, 10);
    JT_CHECK(picked_on(0) == b && k.th[a].rq_on == 0 && clean());
    joist_sched_charge(&k, b, 10);
    JT_CHECK(picked_on(0) == lo && k.th[b].rq_on == 0);
    joist_tick(&k, 99);
    JT_CHECK(k.th[a].rq_on == 0 && clean()); /* not yet */
    joist_tick(&k, 1);
    JT_CHECK(k.th[a].rq_on != 0 && k.th[b].rq_on != 0 && clean());
    JT_CHECK(picked_on(0) == a);
    JT_CHECK(clean());
}

JT_TEST(runq_another_cpus_queue_is_seen)
{
    boot();
    joist_thread lo = thread_with(10, 3, 10, 100);
    joist_thread hi = thread_with(11, 9, 10, 100);
    JT_CHECK(picked_on(1) == hi && k.th[hi].home == 1);
    joist_sched_on(&k, 1, JOIST_MAX_THREADS); /* CPU 1 lets it go */
    JT_CHECK(k.th[hi].rq_on == 2);           /* back on CPU 1's queue */
    JT_CHECK(picked_on(0) == hi && k.th[hi].home == 0 && k.th[hi].rq_on == 0);
    JT_CHECK(picked_on(1) == lo);
    JT_CHECK(clean());
}

JT_TEST(runq_choice_costs_the_same_for_many_threads)
{
    uint64_t cost[2];
    for (unsigned round = 0; round < 2; round++) {
        boot();
        unsigned n = round == 0 ? 2u : (JOIST_MAX_THREADS > 26u ? 24u : JOIST_MAX_THREADS - 2u);
        for (unsigned i = 0; i < n; i++) {
            (void)thread_with((joist_slot)(10u + i), (uint8_t)(i % 7u), 10, 100);
        }
        uint64_t before = k.work;
        JT_CHECK(picked_on(0) != JOIST_MAX_THREADS);
        cost[round] = k.work - before;
    }
    JT_CHECK(cost[0] == cost[1] && cost[0] == JOIST_MAX_CPUS * JOIST_PRIO_WORDS);
    JT_CHECK(clean());
}

/* a, b and c, equals in that order, all waiting for a CPU; a passive server
 * waiting on the endpoint in slot 0. */
static void three_equals_and_a_server(joist_thread *a, joist_thread *b, joist_thread *c,
                                      joist_thread *s)
{
    boot();
    *a = thread_with(10, 5, 100, 100);
    *b = thread_with(11, 5, 100, 100);
    *c = thread_with(12, 5, 100, 100);
    if (joist_thread_create(&k, root, s) != JOIST_OK ||
        joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 0) != JOIST_OK ||
        joist_recv(&k, *s, 0, JOIST_FOREVER) != JOIST_BLOCKED) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
    }
}

JT_TEST(runq_calls_replies_and_yields_take_no_walk)
{
    joist_thread a, b, c, s;
    three_equals_and_a_server(&a, &b, &c, &s);
    JT_CHECK(picked_on(0) == a);
    /* The server takes a's place, the first: at the head of the list. */
    JT_REQUIRE(joist_call(&k, a, 0, &hello, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.rq_walks == 0 && clean());
    JT_CHECK(picked_on(0) == s);
    JT_REQUIRE(joist_reply_recv(&k, s, &hello, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.rq_walks == 0 && picked_on(0) == a && clean());
    /* A yield goes to the back: the tail. */
    JT_CHECK(joist_yield(&k, a) == JOIST_OK && picked_on(0) == b);
    JT_CHECK(k.rq_walks == 0 && k.rq[0].list[5].head == c + 1u && k.rq[0].list[5].tail == a + 1u);
    JT_CHECK(clean());
}

JT_TEST(runq_a_place_in_the_middle_walks_and_is_counted)
{
    joist_thread a, b, c, s;
    three_equals_and_a_server(&a, &b, &c, &s);
    /* None on a CPU: b, second in line, calls; the server takes its place,
     * between a and c, so its insert walks back past c. */
    JT_REQUIRE(joist_call(&k, b, 0, &hello, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.rq_walks == 1);
    JT_CHECK(k.rq[0].list[5].head == a + 1u && k.th[a].rq_next == s + 1u &&
             k.th[s].rq_next == c + 1u && k.rq[0].list[5].tail == c + 1u);
    JT_CHECK(picked_on(0) == a && picked_on(1) == s && picked_on(2) == c);
    JT_CHECK(clean());
}

/* What the kernel only does to the thread on a CPU, the core allows for
 * any: a thread waiting on a queue may yield (to the back of its list) or
 * be charged its whole budget (off the queues). */
JT_TEST(runq_a_queued_thread_yielding_or_spent_is_filed_again)
{
    joist_thread a, b, c, s;
    three_equals_and_a_server(&a, &b, &c, &s);
    JT_CHECK(k.rq[0].list[5].head == a + 1u);
    JT_CHECK(joist_yield(&k, a) == JOIST_OK);
    JT_CHECK(k.rq[0].list[5].head == b + 1u && k.rq[0].list[5].tail == a + 1u && clean());
    /* b runs (its period starts), then waits for a CPU again. */
    JT_CHECK(picked_on(0) == b);
    joist_sched_on(&k, 0, JOIST_MAX_THREADS);
    JT_CHECK(k.rq[0].list[5].head == b + 1u);
    joist_sched_charge(&k, b, 100);
    JT_CHECK(k.th[b].rq_on == 0 && k.rq[0].list[5].head == c + 1u && clean());
    JT_CHECK(picked_on(0) == c);
}

/* The kernel holds the queues for a system call: a passive server woken by
 * a Call and handed the CPU directly never goes on a queue at all. */
JT_TEST(runq_a_held_call_handed_over_files_nothing)
{
    joist_thread a, b, c, s;
    three_equals_and_a_server(&a, &b, &c, &s);
    JT_CHECK(picked_on(0) == a);
    uint64_t place = k.th[a].ready_seq;
    joist_sched_hold(&k);
    JT_REQUIRE(joist_call(&k, a, 0, &hello, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[s].rq_on == 0 && k.th[s].rq_dirty == 1); /* noted, not filed */
    JT_CHECK(joist_sched_better(&k, 1) == 1);               /* (a scan meanwhile) */
    JT_REQUIRE(joist_sched_handed(&k, a, place, s) == s);
    joist_sched_on(&k, 0, s);
    joist_sched_settle(&k);
    JT_CHECK(k.th[s].rq_on == 0 && k.th[s].rq_dirty == 0 && k.rq_npend == 0);
    JT_CHECK(k.rq[0].list[5].head == b + 1u && clean());
    /* Holds nest: an inner settle files nothing. */
    joist_sched_hold(&k);
    joist_sched_hold(&k);
    JT_CHECK(joist_yield(&k, b) == JOIST_OK);
    joist_sched_settle(&k);
    JT_CHECK(k.rq_npend == 1 && k.rq[0].list[5].head == b + 1u);
    joist_sched_settle(&k);
    JT_CHECK(k.rq_npend == 0 && k.rq[0].list[5].head == c + 1u && clean());
}

/* White box: no operation today gives a waiting thread an older place or
 * another home while it is queued, but filing must still put it right if
 * one does (the checks in rq_in_place). */
JT_TEST(runq_filing_moves_a_queued_thread_whose_place_or_home_changed)
{
    joist_thread a, b, c, s;
    three_equals_and_a_server(&a, &b, &c, &s);
    JT_REQUIRE(k.rq[0].list[5].head == a + 1u && k.rq[0].list[5].tail == c + 1u);
    k.th[c].ready_seq = k.th[a].ready_seq - 1u; /* older than everyone's */
    joist__rq_sync(&k, c);
    JT_CHECK(k.rq[0].list[5].head == c + 1u && clean());
    k.th[b].home = 1; /* (as if it had last run on CPU 1) */
    joist__rq_sync(&k, b);
    JT_CHECK(k.th[b].rq_on == 2 && k.rq[1].list[5].head == b + 1u && clean());
}

