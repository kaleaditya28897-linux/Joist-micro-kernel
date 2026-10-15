/* test_sched.c - unit tests for scheduling contexts and their donation
 * (Phase 3 step 3; plan §5.3, §5.6; invariant I11). The root makes a task T
 * with three threads, a client c and passive servers s and s2, and two
 * endpoints in T: EP (served by s) and EP2 (served by s2). The root holds
 * the thread capabilities and c's scheduling context. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

static struct joist_kernel k;
static joist_cspace root, T;
static joist_thread c, s, s2;

/* Root slots. */
enum { R_TASK = 0, R_C = 1, R_S = 2, R_S2 = 3, R_SC = 4, R_SC2 = 5 };
/* T's slots. */
enum { EP = 0, EP2 = 1 };

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

static void setup(void)
{
    joist_init(&k);
    if (joist_boot(&k, 64, &root) != JOIST_OK || joist_task_create(&k, root, 30, R_TASK) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    T = k.obj[k.cs[root].slot[R_TASK].obj - 1u].target - 1u;
    if (joist_thread_spawn(&k, root, R_TASK, R_C, &c) != JOIST_OK ||
        joist_thread_spawn(&k, root, R_TASK, R_S, &s) != JOIST_OK ||
        joist_thread_spawn(&k, root, R_TASK, R_S2, &s2) != JOIST_OK ||
        joist_obj_create(&k, T, JOIST_OBJ_ENDPOINT, EP) != JOIST_OK ||
        joist_obj_create(&k, T, JOIST_OBJ_ENDPOINT, EP2) != JOIST_OK ||
        joist_sc_create(&k, root, R_SC, 10, 5, 10) != JOIST_OK ||
        joist_sc_bind(&k, root, R_SC, R_C) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
    }
}

static uint32_t sc_obj(joist_slot slot) /* the context in a root slot, plus one */
{
    return k.cs[root].slot[slot].obj;
}

JT_TEST(sched_create_errors)
{
    setup();
    JT_CHECK(joist_sc_create(&k, root, 6, 1, 0, 10) == JOIST_E_RANGE);  /* no budget */
    JT_CHECK(joist_sc_create(&k, root, 6, 1, 11, 10) == JOIST_E_RANGE); /* budget > period */
    JT_CHECK(joist_sc_create(&k, root, 6, 1, 1, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_sc_create(&k, root, R_SC, 1, 1, 1) == JOIST_E_OCCUPIED);
    JT_CHECK(joist_sc_create(&k, root, JOIST_CSPACE_SLOTS, 1, 1, 1) == JOIST_E_RANGE);
    JT_CHECK(joist_obj_create(&k, root, JOIST_OBJ_SCHED, 6) == JOIST_E_TYPE); /* needs parameters */
    /* A task's mcp bounds the priorities it may give, and only goes down. */
    JT_REQUIRE(joist_task_set_mcp(&k, root, R_TASK, 20) == JOIST_OK);
    JT_CHECK(joist_sc_create(&k, T, 5, 21, 1, 1) == JOIST_E_RIGHTS);
    JT_CHECK(joist_sc_create(&k, T, 5, 20, 1, 1) == JOIST_OK);
    JT_REQUIRE(joist_task_create(&k, T, 2, 6) == JOIST_OK);
    JT_CHECK(joist_task_set_mcp(&k, T, 6, 21) == JOIST_E_RIGHTS); /* above its own */
    joist_cspace g = k.obj[k.cs[T].slot[6].obj - 1u].target - 1u;
    JT_CHECK(k.cs[g].mcp == 20); /* inherited */
    JT_CHECK(clean());
}

JT_TEST(sched_bind_and_unbind)
{
    setup();
    JT_CHECK(k.th[c].sc == sc_obj(R_SC) && k.th[c].run_sc == sc_obj(R_SC));
    JT_REQUIRE(joist_sc_create(&k, root, R_SC2, 3, 1, 4) == JOIST_OK);
    JT_CHECK(joist_sc_bind(&k, root, R_SC2, R_C) == JOIST_E_OCCUPIED); /* c has one */
    JT_CHECK(joist_sc_bind(&k, root, R_SC, R_S) == JOIST_E_OCCUPIED);  /* the context is bound */
    JT_CHECK(joist_sc_bind(&k, root, R_C, R_S) == JOIST_E_TYPE);
    JT_CHECK(joist_sc_bind(&k, root, R_SC2, R_SC) == JOIST_E_TYPE);
    JT_REQUIRE(joist_cap_derive(&k, root, R_SC2, 7, JOIST_R_GRANT) == JOIST_OK);
    JT_CHECK(joist_sc_bind(&k, root, 7, R_S) == JOIST_E_RIGHTS); /* no CONTROL */
    JT_CHECK(joist_sc_unbind(&k, root, R_SC2) == JOIST_E_STATE); /* not bound */
    JT_REQUIRE(joist_sc_unbind(&k, root, R_SC) == JOIST_OK);
    JT_CHECK(k.th[c].sc == 0 && k.th[c].run_sc == 0);
    JT_REQUIRE(joist_sc_bind(&k, root, R_SC2, R_C) == JOIST_OK);
    JT_CHECK(k.th[c].run_sc == sc_obj(R_SC2));
    JT_REQUIRE(joist_thread_kill(&k, root, R_S2) == JOIST_OK);
    JT_CHECK(joist_sc_bind(&k, root, R_SC, R_S2) == JOIST_E_GONE);
    JT_CHECK(clean());
}

JT_TEST(sched_call_lends_to_passive_server_and_reply_returns)
{
    setup();
    struct joist_msg m = {0};
    uint32_t sc = sc_obj(R_SC);
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, EP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[s].run_sc == sc && k.th[c].run_sc == 0); /* the server runs on c's time */
    JT_CHECK(k.th[c].sc_to == s + 1u && k.th[s].sc_from == c + 1u);
    JT_CHECK(clean());
    JT_REQUIRE(joist_reply(&k, s, &m) == JOIST_OK);
    JT_CHECK(k.th[c].run_sc == sc && k.th[s].run_sc == 0);
    JT_CHECK(k.th[c].sc_to == 0 && k.th[s].sc_from == 0);
    JT_CHECK(clean());
}

JT_TEST(sched_queued_call_lends_at_receive)
{
    setup();
    struct joist_msg m = {0};
    JT_REQUIRE(joist_call(&k, c, EP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[c].run_sc == sc_obj(R_SC)); /* queued: still c's */
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[s].run_sc == sc_obj(R_SC) && k.th[c].run_sc == 0);
    JT_CHECK(clean());
}

JT_TEST(sched_server_with_own_context_borrows_nothing)
{
    setup();
    JT_REQUIRE(joist_sc_create(&k, root, R_SC2, 50, 1, 2) == JOIST_OK);
    JT_REQUIRE(joist_sc_bind(&k, root, R_SC2, R_S) == JOIST_OK);
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, EP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[s].run_sc == sc_obj(R_SC2) && k.th[c].run_sc == sc_obj(R_SC));
    JT_CHECK(k.th[c].sc_to == 0);
    JT_CHECK(clean());
}

JT_TEST(sched_timeout_takes_time_back)
{
    setup();
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, EP, &m, 5) == JOIST_BLOCKED);
    JT_REQUIRE(k.th[s].run_sc == sc_obj(R_SC));
    joist_tick(&k, 10);
    JT_CHECK(k.th[c].state == JOIST_T_RUNNING && k.th[c].result == JOIST_E_TIMEOUT);
    JT_CHECK(k.th[c].run_sc == sc_obj(R_SC) && k.th[s].run_sc == 0 && k.th[s].sc_from == 0);
    JT_CHECK(joist_reply(&k, s, &m) == JOIST_E_PEER); /* the late reply: no time moves */
    JT_CHECK(k.th[c].run_sc == sc_obj(R_SC));
    JT_CHECK(clean());
}

JT_TEST(sched_server_death_returns_time)
{
    setup();
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, EP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_thread_kill(&k, root, R_S) == JOIST_OK);
    JT_CHECK(k.th[c].result == JOIST_E_PEER && k.th[c].run_sc == sc_obj(R_SC));
    JT_CHECK(clean());
}

JT_TEST(sched_deferred_reply_returns_time_at_receive)
{
    /* A server that saves the reply and receives again gives the time back
     * at once: the client, still waiting, holds its own context again. */
    setup();
    JT_REQUIRE(joist_obj_create(&k, T, JOIST_OBJ_REPLY, 2) == JOIST_OK);
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, EP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_reply_save(&k, s, 2) == JOIST_OK);
    JT_CHECK(k.th[s].run_sc == sc_obj(R_SC)); /* still serving */
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[c].run_sc == sc_obj(R_SC) && k.th[c].state == JOIST_T_BLOCKED_REPLY);
    JT_CHECK(k.th[s].run_sc == 0 && k.th[c].sc_to == 0);
    JT_CHECK(clean());
    JT_REQUIRE(joist_reply_saved(&k, s2, 2, &m) == JOIST_OK); /* any thread of T may */
    JT_CHECK(k.th[c].state == JOIST_T_RUNNING && k.th[c].run_sc == sc_obj(R_SC));
    JT_CHECK(clean());
}

JT_TEST(sched_chain_lends_on_and_unwinds)
{
    /* c -> s -> s2, both servers passive: c's context goes down the chain
     * and back up it, one reply at a time. */
    setup();
    uint32_t sc = sc_obj(R_SC);
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, s2, EP2, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, EP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, s, EP2, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[s2].run_sc == sc && k.th[s].run_sc == 0 && k.th[c].run_sc == 0);
    JT_CHECK(k.th[s].sc_from == c + 1u && k.th[s].sc_to == s2 + 1u);
    JT_CHECK(clean());
    JT_REQUIRE(joist_reply(&k, s2, &m) == JOIST_OK);
    JT_CHECK(k.th[s].run_sc == sc && k.th[s2].run_sc == 0 && k.th[c].run_sc == 0);
    JT_CHECK(clean());
    JT_REQUIRE(joist_reply(&k, s, &m) == JOIST_OK);
    JT_CHECK(k.th[c].run_sc == sc && k.th[s].run_sc == 0);
    JT_CHECK(clean());
}

JT_TEST(sched_timeout_pulls_time_back_through_the_chain)
{
    setup();
    uint32_t sc = sc_obj(R_SC);
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, s2, EP2, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, EP, &m, 5) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, s, EP2, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    joist_tick(&k, 10); /* c gives up: its time comes back from the end */
    JT_CHECK(k.th[c].run_sc == sc && k.th[s2].run_sc == 0 && k.th[s].run_sc == 0);
    JT_CHECK(k.th[s].sc_from == 0 && k.th[s].sc_to == 0 && k.th[s2].sc_from == 0);
    JT_CHECK(clean());
    /* s2 still answers s; s wakes with no time of its own (passive). */
    JT_REQUIRE(joist_reply(&k, s2, &m) == JOIST_OK);
    JT_CHECK(k.th[s].state == JOIST_T_RUNNING && k.th[s].run_sc == 0 && k.th[c].run_sc == sc);
    JT_CHECK(clean());
}

JT_TEST(sched_context_deleted_while_lent)
{
    setup();
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, s2, EP2, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, EP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, s, EP2, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_cap_delete(&k, root, R_SC) == JOIST_OK); /* the last capability */
    JT_CHECK(k.th[c].sc == 0 && k.th[c].run_sc == 0 && k.th[c].sc_to == 0);
    JT_CHECK(k.th[s].sc_from == 0 && k.th[s].sc_to == 0 && k.th[s2].run_sc == 0);
    JT_CHECK(clean());
    JT_REQUIRE(joist_reply(&k, s2, &m) == JOIST_OK);
    JT_REQUIRE(joist_reply(&k, s, &m) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(sched_unbind_refused_while_lent)
{
    setup();
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, EP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_sc_unbind(&k, root, R_SC) == JOIST_E_STATE);
    JT_REQUIRE(joist_reply(&k, s, &m) == JOIST_OK);
    JT_CHECK(joist_sc_unbind(&k, root, R_SC) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(sched_client_death_takes_back_and_drops)
{
    setup();
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, EP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_thread_kill(&k, root, R_C) == JOIST_OK);
    JT_CHECK(k.th[s].run_sc == 0 && k.th[s].sc_from == 0);
    JT_CHECK(k.obj[sc_obj(R_SC) - 1u].target == 0); /* the context lives on, unbound */
    JT_CHECK(joist_reply(&k, s, &m) == JOIST_E_PEER);
    JT_CHECK(clean());
}

JT_TEST(sched_invariants_catch_bad_links)
{
    setup();
    char msg[200];
    k.th[s].run_sc = k.th[c].run_sc; /* two threads on one context */
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    k.th[s].run_sc = 0;
    k.th[c].sc_to = s + 1u; /* a loan the borrower doesn't know of */
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    k.th[c].sc_to = 0;
    JT_CHECK(clean());
}
