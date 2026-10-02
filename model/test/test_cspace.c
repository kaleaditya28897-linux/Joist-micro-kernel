/* test_cspace.c - unit tests for capability spaces, objects, derivation,
 * minting and deletion (Phase 1 work item 1; invariants I1, I3, I7, I8). */
#include "harness.h"
#include "joist/model.h"

static struct joist_kernel k;

/* Every test starts from a fresh model and ends with a clean invariant check. */
static void fresh(void)
{
    joist_init(&k);
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

JT_TEST(cspace_create_and_quota)
{
    fresh();
    joist_cspace a;
    JT_REQUIRE(joist_cspace_create(&k, 2, &a) == JOIST_OK);
    JT_CHECK(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_CHECK(joist_obj_create(&k, a, JOIST_OBJ_NOTIFICATION, 1) == JOIST_OK);
    /* I3: the third object exceeds the quota; that is an error, not a block. */
    JT_CHECK(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 2) == JOIST_E_NOMEM);
    JT_CHECK(k.cs[a].charged == 2 && k.objects_live == 2);
    JT_CHECK(clean());
}

JT_TEST(obj_create_errors)
{
    fresh();
    joist_cspace a;
    JT_REQUIRE(joist_cspace_create(&k, 4, &a) == JOIST_OK);
    JT_CHECK(joist_obj_create(&k, a, JOIST_OBJ_NONE, 0) == JOIST_E_TYPE);
    JT_CHECK(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, JOIST_CSPACE_SLOTS) == JOIST_E_RANGE);
    JT_CHECK(joist_obj_create(&k, JOIST_MAX_CSPACES, JOIST_OBJ_ENDPOINT, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_CHECK(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 0) == JOIST_E_OCCUPIED);
    JT_CHECK(k.objects_live == 1);
    JT_CHECK(clean());
}

JT_TEST(derive_never_grows_rights)
{
    fresh();
    joist_cspace a;
    struct joist_cap c;
    JT_REQUIRE(joist_cspace_create(&k, 1, &a) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_CHECK(joist_cap_derive(&k, a, 0, 1, JOIST_R_SEND) == JOIST_OK);
    JT_REQUIRE(joist_cap_read(&k, a, 1, &c) == JOIST_OK);
    JT_CHECK(c.rights == JOIST_R_SEND);
    /* I8: asking for everything from a send-only capability yields send only. */
    JT_CHECK(joist_cap_derive(&k, a, 1, 2, JOIST_R_ALL) == JOIST_OK);
    JT_REQUIRE(joist_cap_read(&k, a, 2, &c) == JOIST_OK);
    JT_CHECK(c.rights == JOIST_R_SEND);
    JT_CHECK(k.obj[c.obj - 1u].refs == 3);
    JT_CHECK(clean());
}

JT_TEST(derive_errors)
{
    fresh();
    joist_cspace a;
    JT_REQUIRE(joist_cspace_create(&k, 1, &a) == JOIST_OK);
    JT_CHECK(joist_cap_derive(&k, a, 0, 1, JOIST_R_ALL) == JOIST_E_EMPTY);
    JT_REQUIRE(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 1) == JOIST_E_NOMEM);
    JT_CHECK(joist_cap_derive(&k, a, 0, 0, JOIST_R_ALL) == JOIST_E_OCCUPIED);
    JT_CHECK(joist_cap_derive(&k, a, 0, JOIST_CSPACE_SLOTS, JOIST_R_ALL) == JOIST_E_RANGE);
    JT_CHECK(clean());
}

JT_TEST(mint_badge_rules)
{
    fresh();
    joist_cspace a;
    struct joist_cap c;
    JT_REQUIRE(joist_cspace_create(&k, 1, &a) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    /* I7: a badge of 0 is "unbadged" and can't be minted. */
    JT_CHECK(joist_cap_mint(&k, a, 0, 1, JOIST_R_SEND, 0) == JOIST_E_BADGE);
    JT_CHECK(joist_cap_mint(&k, a, 0, 1, JOIST_R_SEND, 42) == JOIST_OK);
    JT_REQUIRE(joist_cap_read(&k, a, 1, &c) == JOIST_OK);
    JT_CHECK(c.badge == 42 && c.rights == JOIST_R_SEND);
    /* I7: a badged capability can't be re-badged... */
    JT_CHECK(joist_cap_mint(&k, a, 1, 2, JOIST_R_SEND, 7) == JOIST_E_RIGHTS);
    /* ...and derive keeps the badge it has. */
    JT_CHECK(joist_cap_derive(&k, a, 1, 2, JOIST_R_SEND) == JOIST_OK);
    JT_REQUIRE(joist_cap_read(&k, a, 2, &c) == JOIST_OK);
    JT_CHECK(c.badge == 42);
    JT_CHECK(clean());
}

JT_TEST(mint_needs_grant)
{
    fresh();
    joist_cspace a;
    JT_REQUIRE(joist_cspace_create(&k, 1, &a) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, a, 0, 1, JOIST_R_SEND | JOIST_R_RECV) == JOIST_OK);
    /* I7: without GRANT a holder cannot mint badges, even on an unbadged cap. */
    JT_CHECK(joist_cap_mint(&k, a, 1, 2, JOIST_R_SEND, 9) == JOIST_E_RIGHTS);
    /* A badged capability with GRANT still can't be re-badged. */
    JT_REQUIRE(joist_cap_mint(&k, a, 0, 3, JOIST_R_ALL, 5) == JOIST_OK);
    JT_CHECK(joist_cap_mint(&k, a, 3, 4, JOIST_R_SEND, 6) == JOIST_E_BADGE);
    JT_CHECK(clean());
}

JT_TEST(last_delete_destroys_and_refunds)
{
    fresh();
    joist_cspace a;
    JT_REQUIRE(joist_cspace_create(&k, 1, &a) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, a, 0, 1, JOIST_R_SEND) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, a, 0) == JOIST_OK);
    JT_CHECK(k.objects_live == 1 && k.cs[a].quota == 0);
    JT_CHECK(joist_cap_delete(&k, a, 1) == JOIST_OK);
    /* I1/I3: last capability gone, object gone, quota back. */
    JT_CHECK(k.objects_live == 0 && k.cs[a].quota == 1 && k.cs[a].charged == 0);
    JT_CHECK(joist_cap_delete(&k, a, 1) == JOIST_E_EMPTY);
    JT_CHECK(clean());
}

JT_TEST(destroy_space_frees_everything)
{
    fresh();
    joist_cspace a;
    JT_REQUIRE(joist_cspace_create(&k, 8, &a) == JOIST_OK);
    for (joist_slot i = 0; i < 4; i++) {
        JT_REQUIRE(joist_obj_create(&k, a, JOIST_OBJ_NOTIFICATION, i) == JOIST_OK);
        JT_REQUIRE(joist_cap_derive(&k, a, i, i + 10u, JOIST_R_SEND) == JOIST_OK);
    }
    JT_CHECK(joist_cspace_destroy(&k, a) == JOIST_OK);
    /* I1: nothing leaked; the space is dead and refuses operations. */
    JT_CHECK(k.objects_live == 0);
    JT_CHECK(joist_obj_create(&k, a, JOIST_OBJ_ENDPOINT, 0) == JOIST_E_DEAD);
    JT_CHECK(joist_cspace_destroy(&k, a) == JOIST_E_DEAD);
    JT_CHECK(clean());
}

JT_TEST(object_table_exhaustion_is_an_error)
{
    fresh();
    joist_cspace spaces[JOIST_MAX_CSPACES];
    uint32_t made = 0;
    joist_err last = JOIST_OK;
    /* One space has fewer slots than the table has objects, so fill it across
     * spaces, each with quota to spare: the table is the limit, not quota. */
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES && last == JOIST_OK; s++) {
        JT_REQUIRE(joist_cspace_create(&k, JOIST_CSPACE_SLOTS, &spaces[s]) == JOIST_OK);
        for (joist_slot i = 0; i < JOIST_CSPACE_SLOTS; i++) {
            last = joist_obj_create(&k, spaces[s], JOIST_OBJ_ENDPOINT, i);
            if (last != JOIST_OK) {
                break;
            }
            made++;
        }
    }
    JT_REQUIRE(JOIST_MAX_CSPACES * JOIST_CSPACE_SLOTS > JOIST_MAX_OBJECTS);
    /* I3: exactly the table's capacity was created, then a clean error. */
    JT_CHECK(made == JOIST_MAX_OBJECTS);
    JT_CHECK(last == JOIST_E_NOMEM);
    JT_CHECK(k.objects_live == JOIST_MAX_OBJECTS);
    /* Freeing one object makes room for exactly one more. */
    JT_CHECK(joist_cap_delete(&k, spaces[0], 0) == JOIST_OK);
    JT_CHECK(joist_obj_create(&k, spaces[0], JOIST_OBJ_NOTIFICATION, 0) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(cspace_table_exhaustion)
{
    fresh();
    joist_cspace c = 0;
    for (uint32_t i = 0; i < JOIST_MAX_CSPACES; i++) {
        JT_REQUIRE(joist_cspace_create(&k, 0, &c) == JOIST_OK);
    }
    JT_CHECK(joist_cspace_create(&k, 0, &c) == JOIST_E_NOMEM);
    JT_CHECK(clean());
}
