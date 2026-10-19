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

static struct joist_kernel before;

/* What joist_sched_stays promises: that the scheduler, run now on `cpu`,
 * would choose t and change nothing (but the work it counts). */
static int pick_changes_nothing(uint32_t cpu, joist_thread t)
{
    memcpy(&before, &k, sizeof k);
    if (picked_on(cpu) != t) {
        return 0;
    }
    before.work = k.work;
    return memcmp(&before, &k, sizeof k) == 0;
}

static uint64_t rng;
static uint32_t rnd(uint32_t n)
{
    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (uint32_t)((rng >> 33) % n);
}

/* What the kernel does for a call by the thread on a CPU that changes
 * nothing (joist_sched_stays): it skips the scheduler and keeps its timer.
 * In random worlds, after random operations by that thread, whenever the
 * core says it stays, the scheduler must choose it again and change
 * nothing, and the next event must be no sooner than the timer the kernel
 * set. Sometimes, before the call, another CPU's thread signals first and
 * pokes this CPU, the interrupt still on its way (k->resched already
 * cleared): its events are that CPU's timer's, but a thread it made better
 * for this one must stop t staying. */
JT_TEST(props_a_thread_that_stays_is_the_schedulers_choice)
{
    unsigned long stays = 0, asked = 0;
    for (uint64_t seed = 1; seed <= 300; seed++) {
        rng = seed * 0x9E3779B97F4A7C15ULL;
        boot();
        joist_thread th[6];
        for (uint32_t i = 0; i < 6; i++) {
            uint64_t period = 20u + rnd(40);
            th[i] = thread_with((joist_slot)(10u + i), (uint8_t)(1u + rnd(3)),
                                5u + rnd((uint32_t)period - 4u), period);
        }
        /* Notifications in slots 0 and 1, badged send capabilities in 2, 3. */
        for (joist_slot n = 0; n < 2; n++) {
            JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, n) == JOIST_OK);
            JT_REQUIRE(joist_cap_mint(&k, root, n, (joist_slot)(n + 2u), JOIST_R_SEND, 1u + n) ==
                       JOIST_OK);
        }
        for (int step = 0; step < 400; step++) {
            joist_thread t;
            joist_tick(&k, 0);
            if (joist_sched_pick_on(&k, 0, &t) != JOIST_OK) {
                uint64_t next = joist_sched_next_event(&k, JOIST_MAX_THREADS);
                if (next == JOIST_FOREVER) {
                    break;
                }
                k.now = next;
                continue;
            }
            /* The kernel resumes t, its timer set, and clears resched. */
            uint64_t armed = joist_sched_next_event(&k, t);
            k.resched = 0;
            /* t runs, then enters the kernel: charged first. */
            uint64_t d = rnd(4) == 0 ? rnd(30) : rnd(3);
            k.now += d;
            joist_sched_charge(&k, t, d);
            if (k.now >= armed) {
                continue; /* the timer got there first */
            }
            joist_thread x = th[rnd(6)];
            if (rnd(4) == 0 && x != t && k.th[x].state == JOIST_T_RUNNING) {
                (void)joist_signal(&k, x, (joist_slot)(2u + rnd(2)));
                uint64_t theirs = joist_sched_next_event(&k, t);
                armed = theirs < armed ? theirs : armed;
                k.resched = 0; /* the other CPU has sent its interrupts */
            }
            joist_sched_hold(&k);
            switch (rnd(9)) {
            case 0: (void)joist_signal(&k, t, (joist_slot)(2u + rnd(2))); break;
            case 1: (void)joist_wait(&k, t, (joist_slot)rnd(2), rnd(2) ? 0 : 1u + rnd(40)); break;
            case 2: (void)joist_yield(&k, t); break;
            case 3: (void)joist_cap_delete(&k, root, (joist_slot)(40u + rnd(4))); break;
            case 4: { /* a context taken from another thread, or given back */
                joist_slot s = (joist_slot)(10u + rnd(6));
                if (rnd(2)) {
                    (void)joist_sc_unbind(&k, root, s);
                } else {
                    (void)joist__sc_bind_thread(&k, k.cs[root].slot[s].obj - 1u, th[s - 10u]);
                }
                break;
            }
            case 5: joist_tick(&k, rnd(5)); break;
            default: break; /* a call that changes nothing scheduling sees */
            }
            joist_sched_settle(&k);
            asked++;
            if (joist_sched_stays(&k, 0, t)) {
                stays++;
                uint64_t next = joist_sched_next_event(&k, t);
                if (next < armed || !pick_changes_nothing(0, t)) {
                    fprintf(stderr, "  seed %llu step %d: thread %u stays, but next %llu < armed %llu "
                                    "or the scheduler would choose otherwise or change something\n",
                            (unsigned long long)seed, step, t, (unsigned long long)next,
                            (unsigned long long)armed);
                    JT_CHECK(0);
                    return;
                }
            }
            if (step % 50 == 0 && !clean()) {
                JT_CHECK(0);
                return;
            }
        }
    }
    fprintf(stderr, "  coverage stays: %lu of %lu calls\n", stays, asked);
    JT_CHECK(stays > asked / 10 && stays < asked);
}


/* The system-wide side of joist_sched_stays (and of the direct switch and
 * idle): every pending timed event must be on some CPU's timer. Several
 * CPUs are driven as the kernel drives them: a timer entry (charge, tick,
 * choose afresh), a system call (charge, the call held, then the direct
 * switch keeping the timer, or stays keeping it, or the scheduler and a
 * fresh timer), idle (a timer for the next event anywhere), and the
 * interrupt that asks another CPU to choose again. After every step: each
 * CPU's timer is no later than its own thread's budget end, and the
 * earliest of all the CPUs' timers is no later than the next event
 * anywhere (a blocked thread's timeout, a spent context's new period). */
static uint64_t armed[JOIST_MAX_CPUS];
static uint64_t resumed[JOIST_MAX_CPUS];
static uint32_t ncpus;

static void choose(uint32_t c) /* the scheduler, and a timer for its choice */
{
    joist_thread t;
    if (joist_sched_pick_on(&k, c, &t) == JOIST_OK) {
        armed[c] = joist_sched_next_event(&k, t);
    } else {
        armed[c] = joist_sched_next_event(&k, JOIST_MAX_THREADS); /* idle */
    }
    resumed[c] = k.now;
}

static void charge(uint32_t c)
{
    if (k.on_cpu[c] != 0) {
        joist_sched_charge(&k, k.on_cpu[c] - 1u, k.now - resumed[c]);
    }
    resumed[c] = k.now;
}

/* The interrupt to every CPU that would choose otherwise, until none would.
 * 0 if that never happens: a choice that preempts sets the flag again (0018),
 * so a scheduler that would loop forever is caught here, not hung. */
static int poke(void)
{
    for (uint32_t round = 0; k.resched; round++) {
        if (round > ncpus * JOIST_MAX_THREADS) {
            return 0;
        }
        k.resched = 0;
        for (uint32_t o = 0; o < ncpus; o++) {
            if (joist_sched_better(&k, o)) {
                charge(o);
                choose(o);
            }
        }
    }
    return 1;
}

static int covered(char *why, size_t n)
{
    uint64_t earliest = JOIST_FOREVER;
    for (uint32_t c = 0; c < ncpus; c++) {
        earliest = armed[c] < earliest ? armed[c] : earliest;
        uint32_t t = k.on_cpu[c];
        if (t != 0 && k.th[t - 1u].run_sc != 0) {
            uint64_t end = k.now + k.obj[k.th[t - 1u].run_sc - 1u].remaining;
            if (armed[c] > end) {
                snprintf(why, n, "CPU %u's timer %llu is after its thread %u's budget end %llu", c,
                         (unsigned long long)armed[c], t - 1u, (unsigned long long)end);
                return 0;
            }
        }
    }
    uint64_t next = joist_sched_next_event(&k, JOIST_MAX_THREADS);
    if (earliest > next) {
        snprintf(why, n, "no CPU's timer (earliest %llu) covers the next event %llu",
                 (unsigned long long)earliest, (unsigned long long)next);
        return 0;
    }
    return 1;
}

JT_TEST(props_every_event_is_on_some_cpus_timer)
{
    unsigned long steps = 0, stayed = 0, handed = 0, timers = 0;
    char why[200];
    for (uint64_t seed = 1; seed <= 300; seed++) {
        rng = seed * 0x2545F4914F6CDD1DULL;
        ncpus = 2u + (uint32_t)(seed % 2u) * (JOIST_MAX_CPUS > 2u ? 1u : 0u);
        boot();
        joist_thread th[6];
        for (uint32_t i = 0; i < 6; i++) {
            uint64_t period = 20u + rnd(40);
            th[i] = thread_with((joist_slot)(10u + i), (uint8_t)(1u + rnd(3)),
                                5u + rnd((uint32_t)period - 4u), period);
        }
        for (joist_slot n = 0; n < 2; n++) {
            JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, n) == JOIST_OK);
            JT_REQUIRE(joist_cap_mint(&k, root, n, (joist_slot)(n + 2u), JOIST_R_SEND, 1u + n) ==
                       JOIST_OK);
        }
        joist_thread srv; /* a passive server on the endpoint in slot 4 */
        JT_REQUIRE(joist_thread_create(&k, root, &srv) == JOIST_OK);
        JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 4) == JOIST_OK);
        JT_REQUIRE(joist_recv(&k, srv, 4, JOIST_FOREVER) == JOIST_BLOCKED);
        for (uint32_t c = 0; c < ncpus; c++) {
            choose(c);
        }
        k.resched = 0;
        for (int step = 0; step < 500; step++) {
            /* Time passes; the soonest timer fires when its moment comes. */
            uint32_t first = 0;
            for (uint32_t c = 1; c < ncpus; c++) {
                first = armed[c] < armed[first] ? c : first;
            }
            uint64_t d = rnd(4) == 0 ? rnd(30) : rnd(3);
            if (armed[first] != JOIST_FOREVER && k.now + d >= armed[first]) {
                k.now = armed[first] > k.now ? armed[first] : k.now;
                charge(first);
                joist_tick(&k, 0);
                k.resched = 1; /* a period that has begun may be anyone's (run.c, as there) */
                choose(first);
                if (!poke()) {
                    fprintf(stderr, "  seed %llu step %d: the CPUs never settle\n",
                            (unsigned long long)seed, step);
                    JT_CHECK(0);
                    return;
                }
                timers++;
            } else {
                k.now += d;
                uint32_t c = rnd(ncpus);
                uint32_t on = k.on_cpu[c];
                if (on == 0) {
                    continue; /* idle: nothing enters the kernel there */
                }
                joist_thread t = on - 1u;
                charge(c);
                if (k.th[t].state != JOIST_T_RUNNING) {
                    continue;
                }
                uint64_t place = k.th[t].ready_seq;
                uint32_t to = JOIST_MAX_THREADS;
                int tried = 0;
                joist_sched_hold(&k);
                switch (rnd(9)) {
                case 0: (void)joist_signal(&k, t, (joist_slot)(2u + rnd(2))); break;
                case 1: (void)joist_wait(&k, t, (joist_slot)rnd(2), rnd(2) ? 0 : 1u + rnd(40)); break;
                case 2: (void)joist_yield(&k, t); break;
                case 3: (void)joist_cap_delete(&k, root, (joist_slot)(40u + rnd(4))); break;
                case 4: {
                    joist_slot s = (joist_slot)(10u + rnd(6));
                    if (rnd(2)) {
                        (void)joist_sc_unbind(&k, root, s);
                    } else {
                        (void)joist__sc_bind_thread(&k, k.cs[root].slot[s].obj - 1u, th[s - 10u]);
                    }
                    break;
                }
                case 5:
                case 6:
                    if (k.th[t].reply_to != 0) {
                        to = k.th[t].reply_to - 1u;
                        (void)joist_reply_recv(&k, t, &hello, 4, JOIST_FOREVER);
                        tried = 1;
                    } else if (t != srv && joist_call(&k, t, 4, &hello,
                                                      rnd(4) ? JOIST_FOREVER : 1u + rnd(20)) ==
                                                JOIST_BLOCKED) {
                        to = k.th[t].sc_to != 0 ? k.th[t].sc_to - 1u : JOIST_MAX_THREADS;
                        tried = 1;
                    }
                    break;
                default: break;
                }
                uint32_t h = tried ? joist_sched_handed(&k, t, place, to) : JOIST_MAX_THREADS;
                if (h != JOIST_MAX_THREADS && k.now < armed[c]) {
                    joist_sched_on(&k, c, h); /* the direct switch: the timer stays */
                    joist_sched_settle(&k);
                    k.resched = 0;
                    resumed[c] = k.now;
                    handed++;
                } else {
                    joist_sched_settle(&k);
                    if (k.now < armed[c] && joist_sched_stays(&k, c, t)) {
                        stayed++; /* the timer stays, and no CPU is poked */
                        for (uint32_t o = 0; o < ncpus; o++) {
                            if (joist_sched_better(&k, o)) {
                                fprintf(stderr, "  seed %llu step %d: thread %u stays on CPU %u, but CPU %u "
                                                "would choose otherwise, unpoked\n",
                                        (unsigned long long)seed, step, t, c, o);
                                JT_CHECK(0);
                                return;
                            }
                        }
                        if (!pick_changes_nothing(c, t)) {
                            fprintf(stderr, "  seed %llu step %d: thread %u stays on CPU %u, but the "
                                            "scheduler would choose otherwise or change something\n",
                                    (unsigned long long)seed, step, t, c);
                            JT_CHECK(0);
                            return;
                        }
                    } else {
                        choose(c);
                        if (!poke()) {
                            fprintf(stderr, "  seed %llu step %d: the CPUs never settle\n",
                                    (unsigned long long)seed, step);
                            JT_CHECK(0);
                            return;
                        }
                    }
                }
            }
            steps++;
            if (!covered(why, sizeof why)) {
                fprintf(stderr, "  seed %llu step %d: %s\n", (unsigned long long)seed, step, why);
                JT_CHECK(0);
                return;
            }
            if (step % 100 == 0 && !clean()) {
                JT_CHECK(0);
                return;
            }
        }
    }
    fprintf(stderr, "  coverage timers: %lu steps, %lu stayed, %lu handed over, %lu timer entries\n",
            steps, stayed, handed, timers);
    JT_CHECK(stayed > steps / 20 && handed > 100 && timers > 1000);
}
