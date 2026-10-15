/* test_handover.c - the direct switch (Phase 3 step 8): a Call to a passive
 * server and the reply to it hand the caller's scheduling context over and
 * back, and with it the caller's turn; joist_sched_handed says when the
 * kernel may switch straight to the other thread without asking the
 * scheduler. Unit tests, and the differential test the plan asks for: on
 * random worlds, whenever joist_sched_handed names a thread, joist_sched_pick
 * (the specification) must choose the same one, and the next timer event
 * must be no earlier than before. */
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

static uint32_t picked(void)
{
    joist_thread t;
    return joist_sched_pick(&k, &t) == JOIST_OK ? t : JOIST_MAX_THREADS;
}

static const struct joist_msg hello = {.r = {1, 2, 3, 4}};

/* a and b, equals with contexts of their own (a first in line); a passive
 * server waiting on the endpoint in slot 0. */
static void two_clients_and_a_server(joist_thread *a, joist_thread *b, joist_thread *s)
{
    boot();
    *a = thread_with(10, 5, 100, 100);
    *b = thread_with(11, 5, 100, 100);
    if (joist_thread_create(&k, root, s) != JOIST_OK ||
        joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 0) != JOIST_OK ||
        joist_recv(&k, *s, 0, JOIST_FOREVER) != JOIST_BLOCKED) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
    }
}

JT_TEST(handover_a_call_keeps_the_turn_and_the_reply_returns_it)
{
    joist_thread a, b, s;
    two_clients_and_a_server(&a, &b, &s);
    JT_REQUIRE(picked() == a);
    uint32_t sc = k.th[a].run_sc;
    uint64_t place = k.th[a].ready_seq;
    JT_REQUIRE(joist_call(&k, a, 0, &hello, JOIST_FOREVER) == JOIST_BLOCKED);
    /* The server runs on a's context, in a's place: not behind b. */
    JT_CHECK(joist_sched_handed(&k, a, place, s) == s);
    JT_CHECK(joist_sched_handed(&k, a, place + 1u, s) == JOIST_MAX_THREADS);
    JT_CHECK(k.th[s].run_sc == sc && picked() == s);
    /* (Once s is on a CPU, it is nobody's to switch to.) */
    JT_CHECK(joist_sched_handed(&k, a, place, s) == JOIST_MAX_THREADS);
    JT_REQUIRE(joist_reply_recv(&k, s, &hello, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_sched_handed(&k, s, place, a) == a);
    JT_CHECK(k.th[a].run_sc == sc && picked() == a);
    /* No two threads hold one place. */
    JT_CHECK(k.th[s].ready_seq != k.th[a].ready_seq && k.th[b].ready_seq != k.th[a].ready_seq);
    /* A turn ends when its budget does, as before: then b. */
    joist_sched_charge(&k, a, 100);
    JT_CHECK(picked() == b);
    JT_CHECK(clean());
}

JT_TEST(handover_unsure_when_anything_else_could_matter)
{
    joist_thread a, b, s;
    uint64_t place;
    /* A call with a timeout: its end is a new timer event. */
    two_clients_and_a_server(&a, &b, &s);
    place = k.th[a].ready_seq;
    JT_REQUIRE(joist_call(&k, a, 0, &hello, 50) == JOIST_BLOCKED);
    JT_CHECK(joist_sched_handed(&k, a, place, s) == JOIST_MAX_THREADS);
    /* The budget is spent: the context can't run, whoever holds it. */
    two_clients_and_a_server(&a, &b, &s);
    joist_sched_charge(&k, a, 100);
    place = k.th[a].ready_seq; /* (spending a budget moved it) */
    JT_REQUIRE(joist_call(&k, a, 0, &hello, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_sched_handed(&k, a, place, s) == JOIST_MAX_THREADS);
    /* A server with a context of its own: nothing was handed over. */
    two_clients_and_a_server(&a, &b, &s);
    place = k.th[a].ready_seq;
    JT_REQUIRE(joist_sc_create(&k, root, 12, 5, 100, 100) == JOIST_OK &&
               joist__sc_bind_thread(&k, k.cs[root].slot[12].obj - 1u, s) == JOIST_OK);
    JT_REQUIRE(joist_call(&k, a, 0, &hello, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_sched_handed(&k, a, place, s) == JOIST_MAX_THREADS);
    /* Nobody was waiting: the caller queued, and nobody runs in its place. */
    two_clients_and_a_server(&a, &b, &s);
    place = k.th[a].ready_seq;
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 1) == JOIST_OK);
    JT_REQUIRE(joist_call(&k, a, 1, &hello, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_sched_handed(&k, a, place, s) == JOIST_MAX_THREADS);
    JT_CHECK(joist_sched_handed(&k, a, place, JOIST_MAX_THREADS) == JOIST_MAX_THREADS);
    JT_CHECK(joist_sched_handed(&k, JOIST_MAX_THREADS, place, s) == JOIST_MAX_THREADS);
    JT_CHECK(joist_sched_handed(&k, a, place, s) == JOIST_MAX_THREADS);
    /* A ReplyRecv that finds the next caller waiting: the server goes on,
     * on that caller's context, and the scheduler must choose. */
    two_clients_and_a_server(&a, &b, &s);
    place = k.th[a].ready_seq;
    JT_REQUIRE(joist_call(&k, a, 0, &hello, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, b, 0, &hello, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_reply_recv(&k, s, &hello, 0, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(joist_sched_handed(&k, s, place, a) == JOIST_MAX_THREADS);
    JT_CHECK(clean());
}

/* ---- the differential test ---- */

static uint64_t rng;
static uint32_t rnd(uint32_t n)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return (uint32_t)((rng * 0x2545F4914F6CDD1DULL) >> 33) % n;
}

/* (The small-table build has 8 threads and 32 objects in all: fewer of
 * each there, so that every world is set up as written.) */
#if JOIST_MAX_THREADS >= 16u
#define CLIENTS 6u
#define SERVERS 4u /* each on the endpoint in the slot of its number */
#else
#define CLIENTS 3u
#define SERVERS 2u
#endif

JT_TEST(props_handover_agrees_with_the_scheduler)
{
    unsigned long handed_call = 0, handed_reply = 0, unsure_right = 0, unsure = 0, ops = 0;
    unsigned long timer_first = 0, idle = 0;
    /* Worlds end when everyone waits for ever (servers call each other
     * into knots); as many as it takes for enough operations. */
    for (uint64_t seed = 1; seed <= 5000 && ops < 80000; seed++) {
        rng = seed * 0x9E3779B97F4A7C15ULL;
        boot();
        joist_thread server[SERVERS];
        for (uint32_t i = 0; i < SERVERS; i++) {
            JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, i) == JOIST_OK);
            /* Most servers are passive; in some worlds the last has a
             * context of its own. */
            if (i == SERVERS - 1u && seed % 3u == 0) {
                server[i] = thread_with(40 + i, (uint8_t)(1u + rnd(4)), 20, 30);
            } else {
                JT_REQUIRE(joist_thread_create(&k, root, &server[i]) == JOIST_OK);
            }
            JT_REQUIRE(joist_recv(&k, server[i], i, JOIST_FOREVER) == JOIST_BLOCKED);
        }
        for (uint32_t i = 0; i < CLIENTS; i++) {
            /* Few priorities, so equals are common; budgets below and at
             * the period, so contexts run out and come back. */
            uint64_t period = 20u + rnd(40);
            (void)thread_with(20 + i, (uint8_t)(1u + rnd(3)), 5u + rnd((uint32_t)period - 4u), period);
        }
        for (int step = 0; step < 3000; step++) {
            joist_thread t;
            joist_tick(&k, 0); /* whatever is due */
            if (joist_sched_pick(&k, &t) != JOIST_OK) {
                uint64_t next = joist_sched_next_event(&k, JOIST_MAX_THREADS);
                if (next == JOIST_FOREVER) {
                    break; /* everyone waits for ever: this world is over */
                }
                k.now = next;
                idle++;
                continue;
            }
            /* The kernel resumes t with the timer set for this: */
            uint64_t armed = joist_sched_next_event(&k, t);
            /* t runs a while, then enters the kernel: the clock moves and
             * t is charged, and nothing else has happened yet. */
            uint64_t d = rnd(4) == 0 ? rnd(30) : rnd(3);
            k.now += d;
            joist_sched_charge(&k, t, d);
            if (k.now >= armed) {
                timer_first++; /* the timer got there first: no system call */
                continue;
            }
            struct joist_thread_rec *me = &k.th[t];
            uint32_t sc = me->run_sc;
            uint64_t place = me->ready_seq;
            uint32_t to = JOIST_MAX_THREADS;
            int tried = 0;
            uint32_t ep = rnd(SERVERS);
            int is_server = 0;
            for (uint32_t i = 0; i < SERVERS; i++) {
                is_server |= server[i] == t;
            }
            if (me->reply_to != 0 && rnd(10) < 7) {
                /* A server answering: ReplyRecv on its own endpoint, now
                 * and then with a timeout on the wait. */
                uint32_t mine = 0;
                for (uint32_t i = 0; i < SERVERS; i++) {
                    mine = server[i] == t ? i : mine;
                }
                to = me->reply_to - 1u;
                (void)joist_reply_recv(&k, t, &hello, mine, rnd(8) == 0 ? 10 : JOIST_FOREVER);
                tried = 2;
            } else if (me->reply_to != 0 && rnd(3) == 0) {
                /* Or answering and waiting in two steps. */
                uint32_t mine = 0;
                for (uint32_t i = 0; i < SERVERS; i++) {
                    mine = server[i] == t ? i : mine;
                }
                (void)joist_reply(&k, t, &hello);
                (void)joist_recv(&k, t, mine, JOIST_FOREVER);
            } else if (is_server && me->reply_to == 0) {
                uint32_t mine = 0;
                for (uint32_t i = 0; i < SERVERS; i++) {
                    mine = server[i] == t ? i : mine;
                }
                (void)joist_recv(&k, t, mine, JOIST_FOREVER);
            } else if (rnd(10) < 8 && !(is_server && server[ep] == t)) {
                /* A call: clients to any server, servers on to another
                 * (nested loans), mostly without a timeout. */
                if (joist_call(&k, t, ep, &hello, rnd(8) == 0 ? 15 : JOIST_FOREVER) == JOIST_BLOCKED) {
                    to = me->sc_to != 0 ? me->sc_to - 1u : JOIST_MAX_THREADS;
                }
                tried = 1;
            } /* else: it just ran */
            ops++;
            if (tried) {
                uint32_t h = joist_sched_handed(&k, t, place, to);
                uint32_t spec = picked();
                if (h != JOIST_MAX_THREADS) {
                    uint64_t next = joist_sched_next_event(&k, h);
                    if (h != spec || next < armed) {
                        fprintf(stderr,
                                "  seed %" PRIu64 " step %d: thread %u %s; handed says %u, the "
                                "scheduler %u; next event %" PRIu64 ", was %" PRIu64 "\n",
                                seed, step, t, tried == 1 ? "called" : "replied", h, spec, next, armed);
                        fprintf(stderr, "  now %" PRIu64 " sc %u d %" PRIu64 "\n", k.now, sc, d);
                        for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
                            const struct joist_thread_rec *x = &k.th[i];
                            if (x->state == JOIST_T_FREE) {
                                continue;
                            }
                            const struct joist_object *o = x->run_sc ? &k.obj[x->run_sc - 1u] : NULL;
                            fprintf(stderr,
                                    "    thread %u state %u own %u runs on %u (prio %u left %" PRIu64
                                    " refill %" PRIu64 ") seq %" PRIu64 " from %u to %u deadline %" PRIu64 "\n",
                                    i, x->state, x->sc, x->run_sc, o ? o->prio : 0u,
                                    o ? o->remaining : 0u, o ? o->refill_at : 0u, (uint64_t)x->ready_seq,
                                    x->sc_from, x->sc_to, x->deadline);
                        }
                        JT_CHECK(0);
                        return;
                    }
                    if (tried == 1) {
                        handed_call++;
                    } else {
                        handed_reply++;
                    }
                } else {
                    unsure++;
                    unsure_right += to != JOIST_MAX_THREADS && spec == to;
                }
            }
            if (step % 64 == 0 && !clean()) {
                JT_CHECK(0);
                return;
            }
        }
    }
    fprintf(stderr,
            "  coverage handover: %lu operations; handed on %lu calls and %lu replies; unsure %lu "
            "times (%lu of them the scheduler chose the same); timer first %lu, idle %lu\n",
            ops, handed_call, handed_reply, unsure, unsure_right, timer_first, idle);
    JT_CHECK(handed_call > 5000 && handed_reply > 5000 && unsure > 1000);
}
