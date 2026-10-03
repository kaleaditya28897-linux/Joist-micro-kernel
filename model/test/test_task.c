/* test_task.c - unit tests for boot, tasks and threads as capability-named
 * objects, and task destruction (Phase 1 work item 4; invariants I1, I3;
 * decisions 0004, 0010, 0013). */
#include "harness.h"
#include "internal.h"
#include "joist/model.h"

static struct joist_kernel k;
static joist_cspace root;

/* Every test boots a fresh model with a budget of 64 and ends with a clean
 * invariant check (which includes quota conservation). */
static void boot(void)
{
    joist_init(&k);
    (void)joist_boot(&k, 64, &root);
}

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

/* The space a task capability names. */
static joist_cspace space_of(joist_cspace cs, joist_slot slot)
{
    return k.obj[k.cs[cs].slot[slot].obj - 1u].target - 1u;
}

JT_TEST(boot_root_holds_budget)
{
    joist_init(&k);
    JT_REQUIRE(joist_boot(&k, 64, &root) == JOIST_OK);
    JT_CHECK(k.cs[root].live && k.cs[root].quota == 64 && k.budget == 64);
    JT_CHECK(joist_boot(&k, 64, &root) == JOIST_E_STATE);
    /* 0013: the root task can't be destroyed. */
    JT_CHECK(joist_cspace_destroy(&k, root) == JOIST_E_STATE);
    JT_CHECK(k.cs[root].live);
    JT_CHECK(clean());
}

JT_TEST(task_create_donates_quota)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 10, 0) == JOIST_OK);
    joist_cspace t = space_of(root, 0);
    struct joist_cap c;
    JT_REQUIRE(joist_cap_read(&k, root, 0, &c) == JOIST_OK);
    JT_CHECK(c.type == JOIST_OBJ_TASK && c.rights == (JOIST_R_CONTROL | JOIST_R_GRANT));
    /* The creator pays the donation plus one for the task object (0004). */
    JT_CHECK(k.cs[root].quota == 53 && k.cs[root].charged == 1);
    JT_CHECK(k.cs[t].live && k.cs[t].quota == 10 && k.cs[t].parent == root + 1u);
    JT_CHECK(clean());
}

JT_TEST(task_create_errors)
{
    boot();
    JT_CHECK(joist_task_create(&k, root, 64, 0) == JOIST_E_NOMEM); /* 64 + 1 > 64 */
    JT_CHECK(joist_task_create(&k, root, UINT32_MAX, 0) == JOIST_E_NOMEM);
    JT_CHECK(joist_task_create(&k, root, 1, JOIST_CSPACE_SLOTS) == JOIST_E_RANGE);
    JT_CHECK(joist_task_create(&k, JOIST_MAX_CSPACES, 1, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_task_create(&k, 5, 1, 0) == JOIST_E_DEAD);
    JT_CHECK(joist_task_create(&k, root, 63, 0) == JOIST_OK); /* exactly fits */
    JT_CHECK(joist_task_create(&k, root, 0, 0) == JOIST_E_OCCUPIED);
    JT_CHECK(joist_task_create(&k, root, 0, 1) == JOIST_E_NOMEM);
    /* Failed creations changed nothing. */
    JT_CHECK(k.objects_live == 1 && k.cs[root].quota == 0);
    JT_CHECK(clean());
}

JT_TEST(task_create_space_table_full)
{
    boot();
    joist_slot s = 0;
    while (joist_task_create(&k, root, 0, s) == JOIST_OK) {
        s++;
    }
    JT_CHECK(s == JOIST_MAX_CSPACES - 1u); /* every space but the root */
    JT_CHECK(joist_task_create(&k, root, 0, s) == JOIST_E_NOMEM);
    JT_CHECK(k.objects_live == s);
    JT_CHECK(clean());
}

JT_TEST(task_and_thread_objects_need_their_constructors)
{
    boot();
    JT_CHECK(joist_obj_create(&k, root, JOIST_OBJ_TASK, 0) == JOIST_E_TYPE);
    JT_CHECK(joist_obj_create(&k, root, JOIST_OBJ_THREAD, 0) == JOIST_E_TYPE);
    JT_CHECK(k.objects_live == 0);
    JT_CHECK(clean());
}

JT_TEST(task_destroy_returns_quota_and_charges)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 10, 0) == JOIST_OK);
    joist_cspace t = space_of(root, 0);
    /* The child spends 3: one endpoint it keeps, one it gives the root a
     * copy of (so the object outlives the child), one thread object. */
    JT_REQUIRE(joist_obj_create(&k, t, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, t, JOIST_OBJ_NOTIFICATION, 1) == JOIST_OK);
    JT_REQUIRE(joist_task_create(&k, t, 0, 2) == JOIST_OK);
    JT_CHECK(k.cs[t].quota == 7 && k.cs[t].charged == 3);
    JT_REQUIRE(joist_task_destroy(&k, root, 0) == JOIST_OK);
    JT_CHECK(!k.cs[t].live && k.cs[t].quota == 0 && k.cs[t].charged == 0);
    /* Everything came back: 64 minus the still-held (dead) task object. */
    JT_CHECK(k.cs[root].quota == 63 && k.cs[root].charged == 1);
    JT_CHECK(joist_task_destroy(&k, root, 0) == JOIST_E_GONE);
    /* Deleting the dead task's capability frees its object last. */
    JT_CHECK(joist_cap_delete(&k, root, 0) == JOIST_OK);
    JT_CHECK(k.cs[root].quota == 64 && k.cs[root].charged == 0 && k.objects_live == 0);
    JT_CHECK(clean());
}

JT_TEST(task_destroy_keeps_shared_objects_charged_to_parent)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 4, 0) == JOIST_OK);
    joist_cspace t = space_of(root, 0);
    JT_REQUIRE(joist_obj_create(&k, t, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    /* Give the root a copy, the way a transfer will. */
    JT_REQUIRE(joist__cap_grant(&k, t, 0, root, 1, JOIST_R_ALL) == JOIST_OK);
    JT_REQUIRE(joist_task_destroy(&k, root, 0) == JOIST_OK);
    /* The endpoint lives on in the root, now charged to it. */
    JT_CHECK(k.objects_live == 2);
    JT_CHECK(k.obj[k.cs[root].slot[1].obj - 1u].donor == root);
    JT_CHECK(k.cs[root].charged == 2 && k.cs[root].quota == 62);
    JT_CHECK(clean());
    JT_CHECK(joist_cap_delete(&k, root, 1) == JOIST_OK);
    JT_CHECK(k.cs[root].quota == 63);
    JT_CHECK(clean());
}

JT_TEST(deleting_last_task_cap_destroys_task)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 5, 0) == JOIST_OK);
    joist_cspace t = space_of(root, 0);
    JT_REQUIRE(joist_obj_create(&k, t, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_CHECK(joist_cap_derive(&k, root, 0, 1, JOIST_R_CONTROL) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, root, 0) == JOIST_OK);
    JT_CHECK(k.cs[t].live); /* one capability still names it */
    JT_CHECK(joist_cap_delete(&k, root, 1) == JOIST_OK);
    /* I1: unreachable, so destroyed, and nothing leaks. */
    JT_CHECK(!k.cs[t].live && k.objects_live == 0 && k.cs[root].quota == 64);
    JT_CHECK(clean());
}

JT_TEST(cascade_destroys_unreachable_descendants)
{
    boot();
    /* root -> a -> b -> c, each holding the only capability to the next. */
    JT_REQUIRE(joist_task_create(&k, root, 30, 0) == JOIST_OK);
    joist_cspace a = space_of(root, 0);
    JT_REQUIRE(joist_task_create(&k, a, 20, 0) == JOIST_OK);
    joist_cspace b = space_of(a, 0);
    JT_REQUIRE(joist_task_create(&k, b, 10, 0) == JOIST_OK);
    joist_cspace c = space_of(b, 0);
    JT_REQUIRE(joist_obj_create(&k, c, JOIST_OBJ_NOTIFICATION, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, root, 0) == JOIST_OK);
    JT_CHECK(!k.cs[a].live && !k.cs[b].live && !k.cs[c].live);
    JT_CHECK(k.objects_live == 0 && k.cs[root].quota == 64 && k.npending == 0);
    JT_CHECK(clean());
}

JT_TEST(destroy_reparents_reachable_children)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 30, 0) == JOIST_OK);
    joist_cspace a = space_of(root, 0);
    JT_REQUIRE(joist_task_create(&k, a, 10, 0) == JOIST_OK);
    joist_cspace b = space_of(a, 0);
    /* The root also holds b's task capability, so b outlives a. */
    JT_REQUIRE(joist__cap_grant(&k, a, 0, root, 1, JOIST_R_ALL) == JOIST_OK);
    JT_REQUIRE(joist_task_destroy(&k, root, 0) == JOIST_OK);
    JT_CHECK(!k.cs[a].live && k.cs[b].live && k.cs[b].parent == root + 1u);
    /* b's object was paid for by a; the charge moved to the root. */
    JT_CHECK(k.obj[k.cs[root].slot[1].obj - 1u].donor == root);
    JT_CHECK(clean());
    JT_REQUIRE(joist_task_destroy(&k, root, 1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, root, 0) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, root, 1) == JOIST_OK);
    JT_CHECK(k.objects_live == 0 && k.cs[root].quota == 64);
    JT_CHECK(clean());
}

JT_TEST(task_holding_its_own_cap_is_not_reachable)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 5, 0) == JOIST_OK);
    joist_cspace t = space_of(root, 0);
    /* The task holds a capability to itself: a cycle. */
    JT_REQUIRE(joist__cap_grant(&k, root, 0, t, 0, JOIST_R_ALL) == JOIST_OK);
    JT_REQUIRE(joist_task_destroy(&k, root, 0) == JOIST_OK);
    JT_CHECK(!k.cs[t].live && k.cs[root].quota == 63);
    JT_CHECK(joist_cap_delete(&k, root, 0) == JOIST_OK);
    JT_CHECK(k.objects_live == 0 && k.cs[root].quota == 64);
    JT_CHECK(clean());
}

JT_TEST(grant_errors)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 5, 0) == JOIST_OK);
    joist_cspace t = space_of(root, 0);
    JT_REQUIRE(joist_cap_derive(&k, root, 0, 1, JOIST_R_CONTROL) == JOIST_OK);
    JT_CHECK(joist__cap_grant(&k, root, 1, t, 0, JOIST_R_ALL) == JOIST_E_RIGHTS);
    JT_CHECK(joist__cap_grant(&k, root, 2, t, 0, JOIST_R_ALL) == JOIST_E_EMPTY);
    JT_CHECK(joist__cap_grant(&k, root, 0, 7, 0, JOIST_R_ALL) == JOIST_E_DEAD);
    JT_CHECK(joist__cap_grant(&k, root, 0, t, JOIST_CSPACE_SLOTS, JOIST_R_ALL) == JOIST_E_RANGE);
    JT_CHECK(joist__cap_grant(&k, root, 0, t, 0, JOIST_R_GRANT) == JOIST_OK);
    JT_CHECK(joist__cap_grant(&k, root, 0, t, 0, JOIST_R_ALL) == JOIST_E_OCCUPIED);
    /* I8: the copy has only what the mask allowed. */
    JT_CHECK(k.cs[t].slot[0].rights == JOIST_R_GRANT);
    JT_CHECK(clean());
}

JT_TEST(destroy_rights_and_type)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 5, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, root, 0, 1, JOIST_R_GRANT) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 2) == JOIST_OK);
    JT_CHECK(joist_task_destroy(&k, root, 1) == JOIST_E_RIGHTS);
    JT_CHECK(joist_task_destroy(&k, root, 2) == JOIST_E_TYPE);
    JT_CHECK(joist_task_destroy(&k, root, 3) == JOIST_E_EMPTY);
    JT_CHECK(joist_task_destroy(&k, root, JOIST_CSPACE_SLOTS) == JOIST_E_RANGE);
    JT_CHECK(joist_thread_kill(&k, root, 0) == JOIST_E_TYPE);
    JT_CHECK(k.cs[space_of(root, 0)].live);
    JT_CHECK(clean());
}

JT_TEST(thread_spawn_and_kill)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 5, 0) == JOIST_OK);
    joist_cspace t = space_of(root, 0);
    joist_thread th;
    JT_REQUIRE(joist_thread_spawn(&k, root, 0, 1, &th) == JOIST_OK);
    JT_CHECK(k.th[th].state == JOIST_T_RUNNING && k.th[th].cs == t);
    JT_CHECK(k.cs[root].charged == 2); /* the spawner pays */
    JT_CHECK(clean());
    JT_CHECK(joist_thread_kill(&k, root, 1) == JOIST_OK);
    JT_CHECK(k.th[th].state == JOIST_T_FREE);
    JT_CHECK(joist_thread_kill(&k, root, 1) == JOIST_E_GONE);
    JT_CHECK(clean());
    JT_CHECK(joist_cap_delete(&k, root, 1) == JOIST_OK);
    JT_CHECK(k.cs[root].charged == 1);
    JT_CHECK(clean());
}

JT_TEST(thread_spawn_errors)
{
    boot();
    joist_thread th;
    JT_REQUIRE(joist_task_create(&k, root, 0, 0) == JOIST_OK);
    JT_CHECK(joist_thread_spawn(&k, root, 1, 2, &th) == JOIST_E_EMPTY);
    JT_CHECK(joist_thread_spawn(&k, root, 0, 0, &th) == JOIST_E_OCCUPIED);
    JT_CHECK(joist_thread_spawn(&k, root, 0, JOIST_CSPACE_SLOTS, &th) == JOIST_E_RANGE);
    /* The task has no quota of its own, but the spawner pays. */
    JT_CHECK(joist_thread_spawn(&k, root, 0, 1, &th) == JOIST_OK);
    JT_REQUIRE(joist_task_destroy(&k, root, 0) == JOIST_OK);
    JT_CHECK(joist_thread_spawn(&k, root, 0, 2, &th) == JOIST_E_GONE);
    /* The thread died with its task; its capability says so. */
    JT_CHECK(joist_thread_kill(&k, root, 1) == JOIST_E_GONE);
    JT_CHECK(clean());
}

JT_TEST(thread_spawn_quota_and_table)
{
    joist_init(&k);
    JT_REQUIRE(joist_boot(&k, 2, &root) == JOIST_OK);
    joist_thread th;
    JT_REQUIRE(joist_task_create(&k, root, 0, 0) == JOIST_OK);
    JT_CHECK(joist_thread_spawn(&k, root, 0, 1, &th) == JOIST_OK);
    JT_CHECK(joist_thread_spawn(&k, root, 0, 2, &th) == JOIST_E_NOMEM); /* quota */
    JT_CHECK(clean());

    boot();
    JT_REQUIRE(joist_task_create(&k, root, 0, 0) == JOIST_OK);
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        JT_REQUIRE(joist_thread_create(&k, root, &th) == JOIST_OK);
    }
    JT_CHECK(joist_thread_spawn(&k, root, 0, 1, &th) == JOIST_E_NOMEM); /* table */
    JT_CHECK(k.objects_live == 1);
    JT_CHECK(clean());
}

JT_TEST(deleting_last_thread_cap_kills_thread)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 0, 0) == JOIST_OK);
    joist_thread th;
    JT_REQUIRE(joist_thread_spawn(&k, root, 0, 1, &th) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, root, 1) == JOIST_OK);
    JT_CHECK(k.th[th].state == JOIST_T_FREE && k.objects_live == 1);
    JT_CHECK(clean());
}

JT_TEST(task_destroy_wakes_blocked_peers)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 5, 0) == JOIST_OK);
    joist_cspace t = space_of(root, 0);
    joist_thread srv;
    joist_thread cli;
    /* The server task owns the endpoint; the root's client calls it. */
    JT_REQUIRE(joist_obj_create(&k, t, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_REQUIRE(joist_thread_spawn(&k, root, 0, 1, &srv) == JOIST_OK);
    JT_REQUIRE(joist__cap_grant(&k, t, 0, root, 2, JOIST_R_SEND) == JOIST_OK);
    JT_REQUIRE(joist_thread_create(&k, root, &cli) == JOIST_OK);
    struct joist_msg m = {0};
    JT_REQUIRE(joist_recv(&k, srv, 0, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, cli, 2, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[cli].state == JOIST_T_BLOCKED_REPLY);
    JT_REQUIRE(joist_task_destroy(&k, root, 0) == JOIST_OK);
    /* I10: the server died holding the reply capability; the caller wakes. */
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.th[cli].result == JOIST_E_PEER);
    JT_CHECK(k.th[srv].state == JOIST_T_FREE);
    JT_CHECK(clean());
}

JT_TEST(fixture_space_quota_leaves_budget)
{
    boot();
    joist_cspace f;
    JT_REQUIRE(joist_cspace_create(&k, 3, &f) == JOIST_OK);
    JT_CHECK(k.budget == 67);
    JT_REQUIRE(joist_obj_create(&k, f, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_REQUIRE(joist__cap_grant(&k, f, 0, root, 0, JOIST_R_ALL) == JOIST_OK);
    JT_REQUIRE(joist_cspace_destroy(&k, f) == JOIST_OK);
    /* A parentless space's unspent quota leaves; its live charge stays. */
    JT_CHECK(k.budget == 65 && k.cs[f].charged == 1);
    JT_CHECK(clean());
    JT_CHECK(joist_cap_delete(&k, root, 0) == JOIST_OK);
    JT_CHECK(k.budget == 64);
    JT_CHECK(clean());
}
