/* test_cnode.c - unit tests for the two-level capability table (Phase 3 step
 * 3; 0001 amendment, item 4): leaf 0 built into every space, further leaves
 * added and removed explicitly, charged to quota (I3), and absent leaves
 * out of range. The host build uses JOIST_CNODE_ROOT 2 (model/Makefile), so
 * leaf 1 is the only extra leaf; L is its first slot. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

static struct joist_kernel k;
static joist_cspace root;

#define L JOIST_CNODE_LEAF /* first slot of leaf 1 */

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

JT_TEST(cnode_absent_leaf_is_out_of_range)
{
    boot();
    struct joist_cap c;
    JT_CHECK(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, L) == JOIST_E_RANGE);
    JT_CHECK(joist_cap_read(&k, root, L, &c) == JOIST_E_RANGE);
    JT_CHECK(joist_cap_delete(&k, root, L) == JOIST_E_RANGE);
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_CHECK(joist_cap_derive(&k, root, 0, L, JOIST_R_ALL) == JOIST_E_RANGE);
    JT_CHECK(joist_cap_derive(&k, root, L, 1, JOIST_R_ALL) == JOIST_E_RANGE);
    JT_CHECK(k.cs[root].quota == 63); /* nothing was charged for the refusals */
    JT_CHECK(clean());
}

JT_TEST(cnode_grow_then_use_then_shrink)
{
    boot();
    struct joist_cap c;
    JT_REQUIRE(joist_cspace_grow(&k, root, 1) == JOIST_OK);
    JT_CHECK(k.cs[root].quota == 63 && k.cs[root].charged == 1); /* one unit, like an object */
    JT_CHECK(joist_cap_read(&k, root, L, &c) == JOIST_E_EMPTY);  /* now it exists, empty */
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, L + 5u) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, root, L + 5u, 3, JOIST_R_SEND) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, root, 3, 2u * L - 1u, JOIST_R_SEND) == JOIST_OK);
    JT_CHECK(joist_cap_read(&k, root, 2u * L - 1u, &c) == JOIST_OK && c.rights == JOIST_R_SEND);
    JT_CHECK(k.obj[c.obj - 1u].refs == 3);
    /* A leaf holding capabilities is never removed. */
    JT_CHECK(joist_cspace_shrink(&k, root, 1) == JOIST_E_STATE);
    JT_CHECK(clean());
    JT_REQUIRE(joist_cap_delete(&k, root, L + 5u) == JOIST_OK);
    JT_CHECK(joist_cspace_shrink(&k, root, 1) == JOIST_E_STATE);
    JT_REQUIRE(joist_cap_delete(&k, root, 2u * L - 1u) == JOIST_OK);
    JT_CHECK(joist_cspace_shrink(&k, root, 1) == JOIST_OK);
    JT_CHECK(k.cs[root].quota == 63 && k.cs[root].charged == 1); /* the endpoint in slot 3 */
    JT_CHECK(joist_cap_read(&k, root, L, &c) == JOIST_E_RANGE);
    JT_CHECK(k.leaf[0].owner == 0);
    JT_CHECK(clean());
}

JT_TEST(cnode_grow_and_shrink_errors)
{
    boot();
    JT_CHECK(joist_cspace_grow(&k, root, 0) == JOIST_E_RANGE); /* leaf 0 is built in */
    JT_CHECK(joist_cspace_grow(&k, root, JOIST_CNODE_ROOT) == JOIST_E_RANGE);
    JT_CHECK(joist_cspace_grow(&k, JOIST_MAX_CSPACES, 1) == JOIST_E_RANGE);
    JT_CHECK(joist_cspace_shrink(&k, root, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_cspace_shrink(&k, root, 1) == JOIST_E_RANGE); /* not there */
    JT_REQUIRE(joist_cspace_grow(&k, root, 1) == JOIST_OK);
    JT_CHECK(joist_cspace_grow(&k, root, 1) == JOIST_E_OCCUPIED);
    JT_CHECK(k.cs[root].charged == 1); /* the refused second grow charged nothing */
    joist_cspace dead;
    JT_REQUIRE(joist_cspace_create(&k, 2, &dead) == JOIST_OK);
    JT_REQUIRE(joist_cspace_destroy(&k, dead) == JOIST_OK);
    JT_CHECK(joist_cspace_grow(&k, dead, 1) == JOIST_E_DEAD);
    JT_CHECK(joist_cspace_shrink(&k, dead, 1) == JOIST_E_DEAD);
    JT_CHECK(clean());
}

JT_TEST(cnode_grow_needs_quota_and_pool)
{
    boot();
    joist_cspace s;
    JT_REQUIRE(joist_cspace_create(&k, 0, &s) == JOIST_OK);
    JT_CHECK(joist_cspace_grow(&k, s, 1) == JOIST_E_NOMEM); /* no quota */
    /* Exhaust the pool: one leaf per space, as many spaces as leaves. The
     * host builds have more spaces than leaves (16 against 8, or 2 in the
     * small build), so the pool runs out first. */
    JT_REQUIRE(JOIST_MAX_CSPACES >= JOIST_MAX_LEAVES + 3u);
    for (unsigned n = 0; n < JOIST_MAX_LEAVES; n++) {
        joist_cspace sp;
        JT_REQUIRE(joist_cspace_create(&k, 1, &sp) == JOIST_OK);
        JT_REQUIRE(joist_cspace_grow(&k, sp, 1) == JOIST_OK);
    }
    joist_cspace last;
    JT_REQUIRE(joist_cspace_create(&k, 1, &last) == JOIST_OK);
    JT_CHECK(joist_cspace_grow(&k, last, 1) == JOIST_E_NOMEM); /* pool empty */
    JT_CHECK(k.cs[last].quota == 1);                          /* nothing charged */
    JT_CHECK(clean());
}

JT_TEST(cnode_space_destroy_empties_and_returns_leaves)
{
    boot();
    JT_REQUIRE(joist_task_create(&k, root, 10, 0) == JOIST_OK);
    joist_cspace t = k.obj[k.cs[root].slot[0].obj - 1u].target - 1u;
    JT_REQUIRE(joist_cspace_grow(&k, t, 1) == JOIST_OK);
    /* An endpoint whose only capabilities live in the leaf: it must be freed
     * when the task goes, and the leaf's unit must reach the parent. */
    JT_REQUIRE(joist_obj_create(&k, t, JOIST_OBJ_ENDPOINT, L + 1u) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, t, L + 1u, L + 2u, JOIST_R_SEND) == JOIST_OK);
    JT_CHECK(k.cs[t].quota == 8 && k.cs[t].charged == 2);
    JT_REQUIRE(joist_task_destroy(&k, root, 0) == JOIST_OK);
    JT_CHECK(k.objects_live == 1); /* only the dead task object, still named by the root */
    JT_CHECK(k.cs[t].leaf[1] == 0 && k.leaf[0].owner == 0);
    JT_CHECK(k.cs[root].quota == 63 && k.cs[root].charged == 1);
    JT_REQUIRE(joist_cap_delete(&k, root, 0) == JOIST_OK);
    JT_CHECK(k.cs[root].quota == 64 && k.cs[root].charged == 0);
    JT_CHECK(clean());
}

JT_TEST(cnode_leaf_capability_keeps_task_alive)
{
    /* Reachability and cascades see capabilities in every leaf: a task
     * capability held only in leaf 1 keeps the task alive, and deleting it
     * destroys the task. */
    boot();
    JT_REQUIRE(joist_cspace_grow(&k, root, 1) == JOIST_OK);
    JT_REQUIRE(joist_task_create(&k, root, 4, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, root, 0, L + 7u, JOIST_R_ALL) == JOIST_OK);
    joist_cspace t = k.obj[k.cs[root].slot[0].obj - 1u].target - 1u;
    JT_REQUIRE(joist_cap_delete(&k, root, 0) == JOIST_OK);
    JT_CHECK(k.cs[t].live);
    JT_REQUIRE(joist_cap_delete(&k, root, L + 7u) == JOIST_OK);
    JT_CHECK(!k.cs[t].live);
    JT_CHECK(joist_cspace_shrink(&k, root, 1) == JOIST_OK);
    JT_CHECK(k.cs[root].quota == 64 && k.cs[root].charged == 0);
    JT_CHECK(clean());
}

JT_TEST(cnode_invariants_catch_bad_leaves)
{
    /* The checker itself: a leaf pointing at the wrong space, or a charge
     * missing for a leaf, must be reported (so the property tests mean
     * something for the table). */
    boot();
    char msg[200];
    JT_REQUIRE(joist_cspace_grow(&k, root, 1) == JOIST_OK);
    k.leaf[0].index = 0;
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    k.leaf[0].index = 1;
    k.cs[root].charged = 0;
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    k.cs[root].charged = 1;
    JT_CHECK(clean());
}

JT_TEST(cnode_budget_fits_quota_counters)
{
    /* CBMC (shrink, Phase 3 step 3): a fixture fed past UINT32_MAX made a
     * refund wrap a space's quota. The budget is now capped at UINT32_MAX,
     * so every quota, a share of it, fits. */
    boot();
    joist_cspace s;
    JT_CHECK(joist_cspace_create(&k, UINT32_MAX, &s) == JOIST_E_NOMEM); /* 64 already booted */
    JT_CHECK(k.budget == 64);
    JT_REQUIRE(joist_cspace_create(&k, UINT32_MAX - 64u, &s) == JOIST_OK);
    JT_CHECK(joist_cspace_create(&k, 1, &s) == JOIST_E_NOMEM);
    JT_REQUIRE(joist_cspace_grow(&k, s, 1) == JOIST_OK);
    JT_REQUIRE(joist_cspace_shrink(&k, s, 1) == JOIST_OK);
    JT_CHECK(k.cs[s].quota == UINT32_MAX - 64u);
    JT_CHECK(clean());
}

