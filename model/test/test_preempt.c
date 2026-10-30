/* SPDX-License-Identifier: GPL-3.0-only */
/* test_preempt.c - unit tests for preemptible destruction (Phase 3 step 3;
 * decision 0016; TLA+'s rule, model/tla/IpcDelete.tla). The root makes a
 * task A (root slot RA) holding a few capabilities and the only capability
 * to its own child task B. With k.preempt set, a destruction sweeps that
 * many slots per call and joist_continue does the rest; every intermediate
 * state must pass the invariant check. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

static struct joist_kernel k;
static joist_cspace root, A, B;
static joist_thread ta;

enum { RA = 0 };                              /* root's slots */
enum { A_EP = 0, A_N = 1, A_EP2 = 2, A_B = 6 }; /* A's slots */

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
    if (joist_boot(&k, 64, &root) != JOIST_OK || joist_task_create(&k, root, 30, RA) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    A = k.obj[k.cs[root].slot[RA].obj - 1u].target - 1u;
    if (joist_obj_create(&k, A, JOIST_OBJ_ENDPOINT, A_EP) != JOIST_OK ||
        joist_obj_create(&k, A, JOIST_OBJ_NOTIFICATION, A_N) != JOIST_OK ||
        joist_obj_create(&k, A, JOIST_OBJ_ENDPOINT, A_EP2) != JOIST_OK ||
        joist_task_create(&k, A, 5, A_B) != JOIST_OK ||
        joist_thread_spawn(&k, root, RA, 1, &ta) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
    }
    B = k.obj[k.cs[A].slot[A_B].obj - 1u].target - 1u;
}

static int pending_has(joist_cspace s)
{
    for (uint32_t i = 0; i < k.npending; i++) {
        if (k.pending[i] == s) {
            return 1;
        }
    }
    return 0;
}

JT_TEST(preempt_destroy_in_steps)
{
    setup();
    k.preempt = 1;
    JT_REQUIRE(joist_task_destroy(&k, root, RA) == JOIST_OK);
    /* Begun: dying, its thread gone, on the work list, not yet swept. */
    JT_CHECK(k.cs[A].live && k.cs[A].dying && pending_has(A));
    JT_CHECK(k.th[ta].state == JOIST_T_FREE);
    JT_CHECK(clean());
    unsigned steps = 0;
    joist_err e;
    do {
        e = joist_continue(&k, 1);
        steps++;
        JT_REQUIRE(clean()); /* every intermediate state is consistent */
    } while (e == JOIST_BLOCKED && steps < 10000u);
    JT_CHECK(e == JOIST_OK && steps > 4u);
    JT_CHECK(!k.cs[A].live && !k.cs[B].live && k.npending == 0);
    JT_REQUIRE(joist_cap_delete(&k, root, RA) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, root, 1) == JOIST_OK); /* the dead thread's object */
    JT_CHECK(k.cs[root].quota == 64 && k.objects_live == 0); /* everything came back */
    JT_CHECK(clean());
}

JT_TEST(preempt_dying_space_acts_dead)
{
    setup();
    k.preempt = 1;
    JT_REQUIRE(joist_task_destroy(&k, root, RA) == JOIST_OK);
    /* Every operation on A, and every way of putting a capability into it,
     * is refused (TLA+'s rule). */
    JT_CHECK(joist_obj_create(&k, A, JOIST_OBJ_ENDPOINT, 20) == JOIST_E_DEAD);
    JT_CHECK(joist_cspace_grow(&k, A, 1) == JOIST_E_DEAD);
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, 5) == JOIST_OK);
    JT_CHECK(joist__cap_grant(&k, root, 5, A, 21, JOIST_R_ALL) == JOIST_E_DEAD);
    JT_CHECK(joist_task_reap(&k, root, A) == JOIST_E_DEAD); /* already going */
    /* A is not found as a descendant; B, not yet reached by A's sweep, still
     * is (the root may reap it while A dies). */
    joist_cspace d = JOIST_MAX_CSPACES;
    JT_CHECK(joist_task_descendant(&k, root, 0, &d) == JOIST_OK && d == B);
    joist_thread t;
    JT_CHECK(joist_thread_create(&k, A, &t) == JOIST_E_DEAD);
    struct joist_cap c;
    JT_CHECK(joist_cap_read(&k, A, A_EP, &c) == JOIST_E_DEAD);
    JT_CHECK(clean());
    k.preempt = 0;
    JT_CHECK(joist_continue(&k, 0) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(preempt_cascade_is_swept_newest_first)
{
    /* A's slot A_B holds the only capability to B: sweeping it begins B's
     * destruction at once, on top, and B is swept before A resumes. */
    setup();
    k.preempt = 1;
    JT_REQUIRE(joist_task_destroy(&k, root, RA) == JOIST_OK);
    int saw_both = 0;
    for (unsigned i = 0; i < 10000u && joist_continue(&k, 1) == JOIST_BLOCKED; i++) {
        if (k.cs[B].dying) {
            JT_CHECK(k.cs[A].dying);                           /* A still waits */
            JT_CHECK(k.pending[k.npending - 1u] == B);         /* B on top */
            JT_CHECK(k.cs[A].sweep > A_B);                     /* A resumes after B's slot */
            saw_both = 1;
        }
        JT_REQUIRE(clean());
    }
    JT_CHECK(saw_both && !k.cs[A].live && !k.cs[B].live);
    JT_CHECK(clean());
}

JT_TEST(preempt_unreachable_task_begins_at_once)
{
    /* Deleting the last capability to a task makes it dying in the same
     * operation, even though its sweep waits: no window in which it could
     * still be handed a capability. */
    setup();
    k.preempt = 1;
    JT_REQUIRE(joist_cap_delete(&k, A, A_B) == JOIST_OK);
    JT_CHECK(k.cs[B].dying && pending_has(B));
    JT_CHECK(joist_obj_create(&k, B, JOIST_OBJ_ENDPOINT, 0) == JOIST_E_DEAD);
    JT_CHECK(clean());
    JT_CHECK(joist_continue(&k, 0) == JOIST_OK && !k.cs[B].live);
    JT_CHECK(clean());
}

JT_TEST(preempt_fault_terminates_in_steps)
{
    setup();
    k.preempt = 1;
    JT_CHECK(joist_fault(&k, ta, JOIST_FAULT_GP, 0, 0, 0) == JOIST_E_PEER); /* no handler */
    JT_CHECK(k.cs[A].dying && k.th[ta].state == JOIST_T_FREE);
    JT_CHECK(joist_continue(&k, 0) == JOIST_OK && !k.cs[A].live);
    JT_CHECK(clean());
}

JT_TEST(preempt_checker_catches_a_leak)
{
    /* The leak TLA+ found: a capability put into a slot the sweep has
     * already passed. The checker must see it. */
    setup();
    k.preempt = 2;
    JT_REQUIRE(joist_task_destroy(&k, root, RA) == JOIST_OK);
    JT_REQUIRE(joist_continue(&k, 2) == JOIST_BLOCKED);
    JT_REQUIRE(k.cs[A].sweep > 0);
    k.cs[A].slot[0] = k.cs[root].slot[RA]; /* behind the cursor, by hand */
    char msg[200];
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
}
