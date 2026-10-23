/* test_kill_copy.c - unit tests for revoke by kill and the batched
 * capability copy (decision 0022). The root boots with a 16-page frame in
 * slot F and makes a task (slot TK, space C) to copy into and map into,
 * with a frame of simulated memory (slot MEM) lent to it for its page
 * tables (0021). */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

#include <string.h>

static struct joist_kernel k;
static joist_cspace root, C;

enum { TK = 0, F = 1, F2 = 2, EP = 3, SEND = 4, NOTE = 5, SIG = 6, MEM = 7 }; /* root's slots */

#define MEM_BASE 0x4000u
#define MEM_PAGES 32u
static uint64_t mem[MEM_PAGES][JOIST_PAGE_SIZE / 8u];

static void *page(void *ctx, uint64_t pfn)
{
    (void)ctx;
    return pfn >= MEM_BASE && pfn < MEM_BASE + MEM_PAGES ? mem[pfn - MEM_BASE] : NULL;
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

static void setup(void)
{
    joist_init(&k);
    k.hooks.page = page;
    if (joist_boot(&k, 64, &root) != JOIST_OK ||
        joist_boot_frame(&k, root, F, 0x100, 16) != JOIST_OK ||
        joist_boot_frame(&k, root, MEM, MEM_BASE, MEM_PAGES) != JOIST_OK ||
        joist_task_create(&k, root, 20, TK) != JOIST_OK ||
        joist_space_donate(&k, root, TK, MEM) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    C = k.obj[k.cs[root].slot[TK].obj - 1u].target - 1u;
}

/* Pages mapped from the frame in the root's slot (0020's count). */
static uint64_t mappings_of(uint32_t frame_slot)
{
    return k.obj[k.cs[root].slot[frame_slot].obj - 1u].mapcount;
}

/* ---- obj_kill ---- */

JT_TEST(kill_right_on_boot_frames_and_splits)
{
    setup();
    struct joist_cap c;
    JT_REQUIRE(joist_cap_read(&k, root, F, &c) == JOIST_OK);
    JT_CHECK((c.rights & JOIST_R_KILL) != 0);
    /* A split keeps the rights it had: with KILL, and without. */
    JT_REQUIRE(joist_frame_split(&k, root, F, 8, F2) == JOIST_OK);
    JT_REQUIRE(joist_cap_read(&k, root, F2, &c) == JOIST_OK);
    JT_CHECK(c.rights == (JOIST_R_CONTROL | JOIST_R_WRITE | JOIST_R_KILL | JOIST_R_GRANT));
    JT_REQUIRE(joist_cap_derive(&k, root, F2, 9, JOIST_R_CONTROL | JOIST_R_GRANT) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, root, F2) == JOIST_OK);
    JT_REQUIRE(joist_frame_split(&k, root, 9, 4, 10) == JOIST_OK);
    JT_REQUIRE(joist_cap_read(&k, root, 10, &c) == JOIST_OK);
    JT_CHECK(c.rights == (JOIST_R_CONTROL | JOIST_R_GRANT));
    JT_CHECK(joist_obj_kill(&k, root, 10) == JOIST_E_RIGHTS);
    JT_CHECK(clean());
}

JT_TEST(kill_errors_in_order)
{
    setup();
    JT_CHECK(joist_obj_kill(&k, JOIST_MAX_CSPACES, F) == JOIST_E_RANGE);
    JT_CHECK(joist_obj_kill(&k, root, JOIST_CSPACE_SLOTS) == JOIST_E_RANGE);
    JT_CHECK(joist_obj_kill(&k, root, JOIST_CNODE_LEAF) == JOIST_E_RANGE); /* absent leaf */
    JT_CHECK(joist_obj_kill(&k, root, 9) == JOIST_E_EMPTY);
    JT_CHECK(joist_obj_kill(&k, root, TK) == JOIST_E_TYPE);
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, EP) == JOIST_OK);
    JT_CHECK(joist_obj_kill(&k, root, EP) == JOIST_E_TYPE); /* endpoints die their own way */
    /* Without the kill right: refused, nothing changes. */
    JT_REQUIRE(joist_cap_derive(&k, root, F, 9, (joist_rights)~JOIST_R_KILL) == JOIST_OK);
    JT_CHECK(joist_obj_kill(&k, root, 9) == JOIST_E_RIGHTS);
    JT_CHECK(!k.obj[k.cs[root].slot[F].obj - 1u].dead);
    JT_CHECK(joist_obj_kill(&k, root, F) == JOIST_OK);
    JT_CHECK(joist_obj_kill(&k, root, F) == JOIST_E_GONE);
    JT_CHECK(joist_obj_kill(&k, root, 9) == JOIST_E_RIGHTS); /* rights first, as everywhere */
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK);
    JT_CHECK(joist_obj_kill(&k, C, 0) == JOIST_E_DEAD);
    JT_CHECK(clean());
}

JT_TEST(kill_every_copy_fails_gone)
{
    setup();
    /* Copies in the root and in the child, one without the kill right. */
    JT_REQUIRE(joist_cap_derive(&k, root, F, 9, (joist_rights)~JOIST_R_KILL) == JOIST_OK);
    JT_REQUIRE(joist_cap_give(&k, root, TK, F, 4, JOIST_R_ALL) == JOIST_OK);
    uint32_t obj = k.cs[root].slot[F].obj - 1u;
    uint32_t live = k.objects_live;
    JT_CHECK(joist_obj_kill(&k, root, F) == JOIST_OK);
    JT_CHECK(k.obj[obj].dead && k.obj[obj].refs == 3 && k.objects_live == live); /* I1 */
    JT_CHECK(clean());
    /* Every use, from every copy, any space. */
    JT_CHECK(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_E_GONE);
    JT_CHECK(joist_map(&k, root, 9, 0, 1, TK, 0x400, 0) == JOIST_E_GONE);
    JT_CHECK(joist_map(&k, C, 4, 0, 1, 0, 0x400, JOIST_MAP_W) == JOIST_E_GONE); /* the frame first */
    JT_CHECK(joist_space_donate(&k, root, TK, 9) == JOIST_E_GONE);
    JT_CHECK(joist_frame_split(&k, root, 9, 4, 10) == JOIST_E_GONE);
    JT_CHECK(joist_frame_split(&k, C, 4, 4, 10) == JOIST_E_GONE);
    JT_CHECK(joist_obj_kill(&k, C, 4) == JOIST_E_GONE);
    /* The capabilities themselves still move and go, as a dead endpoint's
     * do: a copy of a dead frame is dead too. */
    JT_CHECK(joist_cap_derive(&k, C, 4, 5, JOIST_R_ALL) == JOIST_OK);
    JT_CHECK(joist_map(&k, C, 5, 0, 1, 0, 0x400, 0) == JOIST_E_GONE);
    /* Its pages stay its own until its last capability goes (no page is in
     * two frames, I12), and its unit stays charged to the root until then. */
    JT_CHECK(joist_boot_frame(&k, root, 10, 0x100, 1) == JOIST_E_STATE);
    uint32_t q = k.cs[root].quota;
    JT_CHECK(joist_cap_delete(&k, root, F) == JOIST_OK &&
             joist_cap_delete(&k, root, 9) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, C, 4) == JOIST_OK);
    JT_CHECK(k.obj[obj].type == JOIST_OBJ_FRAME && k.cs[root].quota == q);
    JT_CHECK(joist_cap_delete(&k, C, 5) == JOIST_OK);
    JT_CHECK(k.obj[obj].type == JOIST_OBJ_NONE && k.cs[root].quota == q + 1u);
    JT_CHECK(k.objects_live == live - 1u);
    JT_CHECK(joist_boot_frame(&k, root, 10, 0x100, 1) == JOIST_OK); /* its pages are free again */
    JT_CHECK(clean());
}

JT_TEST(kill_unmaps_everywhere)
{
    setup();
    JT_REQUIRE(joist_frame_split(&k, root, F, 8, F2) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, root, F, 9, JOIST_R_ALL) == JOIST_OK);
    /* F's pages at three places in the child, two through a copy; F2 once,
     * in a last-level table of its own. */
    JT_REQUIRE(joist_map(&k, root, F, 0, 2, TK, 0x400, JOIST_MAP_W) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 3, 1, TK, 0x600, 0) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, 9, 4, 1, TK, 0x410, JOIST_MAP_X) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F2, 0, 1, TK, 0x800, 0) == JOIST_OK);
    JT_CHECK(mappings_of(F) == 4 && mappings_of(F2) == 1 && k.cs[C].pt_used == 10);
    JT_CHECK(joist_obj_kill(&k, root, 9) == JOIST_OK); /* through the copy */
    JT_CHECK(mappings_of(F) == 0 && mappings_of(F2) == 1); /* only the frame killed */
    JT_CHECK(k.frames_dying == 0 && k.cs[C].pt_used == 6); /* its tables given back */
    JT_CHECK(joist_unmap_range(&k, root, TK, 0x400, 2) == JOIST_OK && mappings_of(F2) == 1);
    JT_CHECK(joist_unmap_range(&k, root, TK, 0x800, 1) == JOIST_OK && mappings_of(F2) == 0);
    JT_CHECK(clean());
}

JT_TEST(kill_sweeps_a_table_per_step)
{
    /* Preempted, the kill returns at once; its sweep goes on a table at a
     * time, the frame dead throughout. */
    setup();
    JT_REQUIRE(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 1, 1, TK, 0x600, 0) == JOIST_OK);
    k.preempt = 1;
    JT_CHECK(joist_obj_kill(&k, root, F) == JOIST_OK);
    JT_CHECK(k.obj[k.cs[root].slot[F].obj - 1u].dead && k.frames_dying == 1);
    JT_CHECK(joist_map(&k, root, F, 2, 1, TK, 0x402, 0) == JOIST_E_GONE);
    JT_CHECK(clean());
    unsigned steps = 0;
    while (joist_continue(&k, 1) == JOIST_BLOCKED && steps < 100000u) {
        steps++;
        JT_CHECK(clean());
    }
    k.preempt = 0;
    JT_CHECK(steps > 1 && k.frames_dying == 0 && mappings_of(F) == 0);
    /* Its last capability gone, it is freed: nothing keeps it. */
    uint32_t obj = k.cs[root].slot[F].obj - 1u;
    JT_REQUIRE(joist_cap_delete(&k, root, F) == JOIST_OK);
    JT_CHECK(k.obj[obj].type == JOIST_OBJ_NONE);
    JT_CHECK(clean());
}

JT_TEST(killed_frame_losing_its_last_capability_mid_sweep_counts_once)
{
    /* Killed, and so counted; its last capability then goes while the sweep
     * still has its mappings to clear: still one frame for the sweep, which
     * frees it at the end. */
    setup();
    JT_REQUIRE(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 1, 1, TK, 0x600, 0) == JOIST_OK);
    k.preempt = 1;
    JT_CHECK(joist_obj_kill(&k, root, F) == JOIST_OK && k.frames_dying == 1);
    uint32_t obj = k.cs[root].slot[F].obj - 1u;
    JT_REQUIRE(joist_cap_delete(&k, root, F) == JOIST_OK);
    JT_CHECK(k.frames_dying == 1 && k.obj[obj].mapcount != 0);
    JT_CHECK(clean());
    unsigned steps = 0;
    while (joist_continue(&k, 1) == JOIST_BLOCKED && steps < 100000u) {
        steps++;
    }
    k.preempt = 0;
    JT_CHECK(k.frames_dying == 0 && k.obj[obj].type == JOIST_OBJ_NONE);
    JT_CHECK(clean());
}

JT_TEST(kill_then_task_destroy_and_teardown)
{
    /* A killed frame held only by a child goes with the child. */
    setup();
    JT_REQUIRE(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_give(&k, root, TK, F, 4, JOIST_R_ALL) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, root, F) == JOIST_OK);
    JT_CHECK(joist_obj_kill(&k, C, 4) == JOIST_OK);
    JT_CHECK(k.obj[k.cs[C].slot[4].obj - 1u].mapcount == 0);
    JT_CHECK(clean());
    uint32_t live = k.objects_live;
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK);
    JT_CHECK(k.objects_live == live - 1u); /* the frame; the dead task object stays */
    JT_CHECK(joist_cap_delete(&k, root, TK) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, root, MEM) == JOIST_OK); /* not lent any more: freed */
    JT_CHECK(k.objects_live == 0 && k.cs[root].quota == 64u);
    JT_CHECK(clean());
}

/* ---- cap_copy_batch ---- */

static uint32_t done, skipped;

static joist_err batch(joist_slot first, uint32_t count, joist_rights mask, uint32_t flags)
{
    done = 0xdead;
    skipped = 0xdead;
    return joist_cap_copy_batch(&k, root, TK, first, count, mask, flags, &done, &skipped);
}

/* The root's endpoint (full rights) in EP, a badged send copy in SEND, a
 * notification in NOTE and a badged copy of it in SIG. */
static void some_caps(void)
{
    if (joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, EP) != JOIST_OK ||
        joist_cap_mint(&k, root, EP, SEND, JOIST_R_SEND | JOIST_R_GRANT, 7) != JOIST_OK ||
        joist_obj_create(&k, root, JOIST_OBJ_NOTIFICATION, NOTE) != JOIST_OK ||
        joist_cap_mint(&k, root, NOTE, SIG, JOIST_R_ALL, 0x10) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the capabilities could not be made as written", 0);
    }
}

JT_TEST(batch_copies_same_slots_masked_badge_kept)
{
    setup();
    some_caps();
    uint32_t refs = k.obj[k.cs[root].slot[SEND].obj - 1u].refs;
    JT_CHECK(batch(0, 8, (joist_rights)~JOIST_R_KILL, 0) == JOIST_OK);
    JT_CHECK(done == 8);
    /* EP has RECV: skipped (no flag). Everything else with GRANT copied. */
    JT_CHECK(skipped == 1u << EP);
    for (uint32_t s = 0; s < 8; s++) {
        const struct joist_cap *p = &k.cs[root].slot[s];
        const struct joist_cap *c = &k.cs[C].slot[s];
        if (s == EP || p->obj == 0) {
            JT_CHECK(c->obj == 0);
            continue;
        }
        JT_CHECK(c->obj == p->obj && c->type == p->type && c->badge == p->badge);
        JT_CHECK(c->rights == (p->rights & (joist_rights)~JOIST_R_KILL)); /* I8 */
    }
    JT_CHECK(k.cs[C].slot[F].rights == (JOIST_R_CONTROL | JOIST_R_WRITE | JOIST_R_GRANT));
    JT_CHECK(k.obj[k.cs[root].slot[SEND].obj - 1u].refs == refs + 1u);
    /* The child's own task capability came along: a task may hold its own. */
    JT_CHECK(k.cs[C].slot[TK].obj == k.cs[root].slot[TK].obj);
    JT_CHECK(clean());
}

JT_TEST(batch_skips_no_grant_and_receive_rights)
{
    setup();
    some_caps();
    JT_REQUIRE(joist_cap_derive(&k, root, EP, 8, JOIST_R_SEND | JOIST_R_RECV) ==
               JOIST_OK);                                                      /* no GRANT */
    JT_REQUIRE(joist_cap_derive(&k, root, NOTE, 9, JOIST_R_SEND) == JOIST_OK); /* no GRANT */
    uint32_t ep = k.cs[root].slot[EP].obj - 1u;
    JT_CHECK(batch(EP, 7, JOIST_R_ALL, 0) == JOIST_OK && done == 7);
    JT_CHECK(skipped == (1u << 0 | 1u << 5 | 1u << 6)); /* EP (RECV), 8 and 9 (no GRANT) */
    JT_CHECK(k.cs[C].slot[EP].obj == 0 && k.cs[C].slot[8].obj == 0 && k.cs[C].slot[9].obj == 0);
    /* A notification's RECV is no receive right: NOTE was copied whole. */
    JT_CHECK(k.cs[C].slot[NOTE].rights == (JOIST_R_SEND | JOIST_R_RECV | JOIST_R_GRANT));
    /* A mask without RECV makes a send right of it, which is copied. */
    JT_CHECK(batch(EP, 1, JOIST_R_SEND | JOIST_R_GRANT, 0) == JOIST_OK && done == 1 &&
             skipped == 0);
    JT_CHECK(k.cs[C].slot[EP].rights == (JOIST_R_SEND | JOIST_R_GRANT));
    JT_CHECK(k.obj[ep].recv_caps == 2); /* the root's EP and 8 */
    JT_REQUIRE(joist_cap_delete(&k, C, EP) == JOIST_OK);
    /* Asked for: the receive right too, counted (I5). */
    JT_CHECK(batch(EP, 1, JOIST_R_ALL, JOIST_COPY_RECV) == JOIST_OK && done == 1 && skipped == 0);
    JT_CHECK(k.cs[C].slot[EP].rights == (JOIST_R_SEND | JOIST_R_RECV | JOIST_R_GRANT));
    JT_CHECK(k.obj[ep].recv_caps == 3);
    /* The parent's receive rights gone, the child's keeps the endpoint alive. */
    JT_REQUIRE(joist_cap_delete(&k, root, EP) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, root, 8) == JOIST_OK);
    JT_CHECK(!k.obj[ep].dead && k.obj[ep].recv_caps == 1);
    JT_CHECK(clean());
}

JT_TEST(batch_stops_at_occupied_keeping_earlier_copies)
{
    setup();
    some_caps();
    JT_REQUIRE(joist_obj_create(&k, C, JOIST_OBJ_ENDPOINT, NOTE) == JOIST_OK); /* in the way */
    struct joist_cap before = k.cs[C].slot[NOTE];
    JT_CHECK(batch(0, 10, JOIST_R_ALL, 0) == JOIST_E_OCCUPIED);
    JT_CHECK(done == NOTE && skipped == 1u << EP);
    JT_CHECK(k.cs[C].slot[TK].obj != 0 && k.cs[C].slot[F].obj != 0 && k.cs[C].slot[SEND].obj != 0);
    JT_CHECK(k.cs[C].slot[SIG].obj == 0); /* nothing past the stop */
    JT_CHECK(k.cs[C].slot[NOTE].obj == before.obj && k.cs[C].slot[NOTE].rights == before.rights);
    JT_CHECK(clean());
    /* The caller clears the way and goes on from first + done. */
    JT_REQUIRE(joist_cap_delete(&k, C, NOTE) == JOIST_OK);
    JT_CHECK(batch(done, 10 - NOTE, JOIST_R_ALL, 0) == JOIST_OK && done == 10 - NOTE &&
             skipped == 0);
    JT_CHECK(k.cs[C].slot[NOTE].obj == k.cs[root].slot[NOTE].obj && k.cs[C].slot[SIG].obj != 0);
    /* A skipped capability's target is never looked at. */
    JT_REQUIRE(joist_obj_create(&k, C, JOIST_OBJ_ENDPOINT, EP) == JOIST_OK);
    JT_CHECK(batch(EP, 1, JOIST_R_ALL, 0) == JOIST_OK && done == 1 && skipped == 1u);
    JT_CHECK(clean());
}

JT_TEST(batch_stops_at_a_missing_leaf_then_goes_on)
{
    setup();
    some_caps();
    joist_slot s0 = JOIST_CNODE_LEAF - 2u; /* two slots in leaf 0, two in leaf 1 */
    JT_REQUIRE(joist_cspace_grow(&k, root, 1) == JOIST_OK);
    for (uint32_t i = 0; i < 4; i++) {
        JT_REQUIRE(joist_cap_derive(&k, root, SEND, s0 + i, JOIST_R_ALL) == JOIST_OK);
    }
    JT_CHECK(batch(s0, 4, JOIST_R_ALL, 0) == JOIST_E_RANGE && done == 2 && skipped == 0);
    JT_CHECK(k.cs[C].slot[s0].obj != 0 && k.cs[C].slot[s0 + 1u].obj != 0);
    JT_CHECK(clean());
    uint32_t q = k.cs[C].quota;
    JT_REQUIRE(joist_cspace_grow(&k, C, 1) == JOIST_OK); /* the child pays */
    JT_CHECK(k.cs[C].quota == q - 1u);
    JT_CHECK(batch(s0 + done, 2, JOIST_R_ALL, 0) == JOIST_OK && done == 2);
    JT_CHECK(joist__slot_c(&k, C, JOIST_CNODE_LEAF + 1u)->obj == k.cs[root].slot[SEND].obj);
    /* An absent leaf of the caller's own holds nothing, and so stops nothing. */
    JT_REQUIRE(joist_cap_delete(&k, root, s0 + 2u) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, root, s0 + 3u) == JOIST_OK);
    JT_REQUIRE(joist_cspace_shrink(&k, root, 1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, C, s0 + 2u) == JOIST_OK);
    JT_CHECK(batch(s0 + 2u, 2, JOIST_R_ALL, 0) == JOIST_OK && done == 2 && skipped == 0);
    JT_CHECK(clean());
}

JT_TEST(batch_refusals_change_nothing)
{
    setup();
    some_caps();
    static struct joist_kernel snap;
    memcpy(&snap, &k, sizeof k);
    JT_CHECK(batch(0, JOIST_COPY_BATCH + 1u, JOIST_R_ALL, 0) == JOIST_E_RANGE && done == 0 &&
             skipped == 0);
    JT_CHECK(batch(JOIST_CSPACE_SLOTS - 4u, 5, JOIST_R_ALL, 0) == JOIST_E_RANGE);
    JT_CHECK(batch(JOIST_CSPACE_SLOTS, 0, JOIST_R_ALL, 0) == JOIST_E_RANGE);
    JT_CHECK(batch(UINT32_MAX - 1u, 4, JOIST_R_ALL, 0) == JOIST_E_RANGE); /* no wrap */
    JT_CHECK(batch(0, 4, JOIST_R_ALL, 2u) == JOIST_E_RANGE);              /* unknown flag */
    JT_CHECK(batch(0, 0, JOIST_R_ALL, 0) == JOIST_OK && done == 0 && skipped == 0);
    JT_CHECK(joist_cap_copy_batch(&k, JOIST_MAX_CSPACES, TK, 0, 4, JOIST_R_ALL, 0, &done,
                                  &skipped) == JOIST_E_RANGE);
    JT_CHECK(joist_cap_copy_batch(&k, root, JOIST_CSPACE_SLOTS, 0, 4, JOIST_R_ALL, 0, &done,
                                  &skipped) == JOIST_E_RANGE);
    JT_CHECK(joist_cap_copy_batch(&k, root, 20, 0, 4, JOIST_R_ALL, 0, &done, &skipped) ==
             JOIST_E_EMPTY);
    JT_CHECK(joist_cap_copy_batch(&k, root, EP, 0, 4, JOIST_R_ALL, 0, &done, &skipped) ==
             JOIST_E_TYPE);
    /* CONTROL on the task is needed: GRANT alone is not enough. */
    JT_REQUIRE(joist_cap_derive(&k, root, TK, 20, JOIST_R_GRANT) == JOIST_OK);
    JT_CHECK(joist_cap_copy_batch(&k, root, 20, 0, 4, JOIST_R_ALL, 0, &done, &skipped) ==
             JOIST_E_RIGHTS);
    JT_REQUIRE(joist_cap_delete(&k, root, 20) == JOIST_OK);
    k.work = snap.work; /* (the work counter is not state) */
    JT_CHECK(memcmp(&snap, &k, sizeof k) == 0);
    JT_CHECK(clean());
}

JT_TEST(batch_into_a_dying_or_dead_child)
{
    setup();
    some_caps();
    /* Dying: its destruction half done (0016). Nothing may go in (TLA+'s
     * rule), whatever the capability, and nothing changes. */
    for (uint32_t i = 0; i < 6; i++) {
        JT_REQUIRE(joist_cap_give(&k, root, TK, SEND, 10 + i, JOIST_R_ALL) == JOIST_OK);
    }
    JT_REQUIRE(joist_cap_derive(&k, root, TK, 20, JOIST_R_ALL) == JOIST_OK);
    k.preempt = 2;
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK);
    JT_REQUIRE(k.cs[C].dying && k.npending == 1);
    JT_CHECK(batch(0, 8, JOIST_R_ALL, 0) == JOIST_E_DEAD && done == 0 && skipped == 0);
    JT_CHECK(joist_cap_copy_batch(&k, root, 20, 0, 8, JOIST_R_ALL, 0, &done, &skipped) ==
             JOIST_E_DEAD);
    JT_CHECK(k.cs[C].slot[F].obj == 0 && k.cs[C].slot[SEND].obj == 0);
    JT_CHECK(clean());
    k.preempt = 0;
    JT_REQUIRE(joist_continue(&k, 0) == JOIST_OK);
    /* Dead: still E_DEAD, and its space index may be someone else's now. */
    JT_CHECK(batch(0, 8, JOIST_R_ALL, 0) == JOIST_E_DEAD);
    JT_REQUIRE(joist_task_create(&k, root, 4, 21) == JOIST_OK);
    JT_REQUIRE(k.obj[k.cs[root].slot[21].obj - 1u].target - 1u == C); /* reused */
    JT_CHECK(batch(0, 8, JOIST_R_ALL, 0) == JOIST_E_DEAD);
    JT_CHECK(k.cs[C].slot[F].obj == 0);
    JT_CHECK(clean());
}

JT_TEST(batch_never_allocates_and_keeps_no_senders)
{
    setup();
    some_caps();
    JT_REQUIRE(joist_bind_nosenders(&k, root, EP, SIG) == JOIST_OK);
    /* Fill the no-senders account table: the batch still copies badged
     * send capabilities, since their account exists. */
    JT_REQUIRE(joist_cspace_grow(&k, root, 1) == JOIST_OK);
    uint32_t n = 0;
    while (n < JOIST_CSPACE_SLOTS - 30u &&
           joist_cap_mint(&k, root, EP, 30 + n, JOIST_R_SEND, 100 + n) == JOIST_OK) {
        n++;
    }
    JT_REQUIRE(n == JOIST_MAX_BADGE_ACCTS - 1u); /* badge 7 has the other */
    JT_CHECK(joist_cap_mint(&k, root, EP, 29, JOIST_R_SEND, 99) == JOIST_E_NOMEM);
    uint32_t live = k.objects_live;
    uint32_t rq = k.cs[root].quota;
    uint32_t cq = k.cs[C].quota;
    JT_CHECK(batch(SEND, 1, JOIST_R_ALL, 0) == JOIST_OK && done == 1);
    JT_CHECK(k.objects_live == live && k.cs[root].quota == rq && k.cs[C].quota == cq);
    JT_CHECK(clean());
    /* No-senders for badge 7 waits for the child's copy too. */
    joist_badge got;
    JT_REQUIRE(joist_cap_delete(&k, root, SEND) == JOIST_OK);
    JT_CHECK(joist_nosenders_next(&k, root, EP, &got) == JOIST_OK && got == 0);
    JT_REQUIRE(joist_cap_delete(&k, C, SEND) == JOIST_OK);
    JT_CHECK(joist_nosenders_next(&k, root, EP, &got) == JOIST_OK && got == 7);
    JT_CHECK(clean());
}

JT_TEST(batch_work_is_bounded_by_the_batch)
{
    setup();
    some_caps();
    for (uint32_t i = 0; i < JOIST_COPY_BATCH; i++) {
        JT_REQUIRE(joist_cap_derive(&k, root, SEND, 10 + i, JOIST_R_ALL) == JOIST_OK);
    }
    uint64_t w0 = k.work;
    JT_CHECK(batch(10, JOIST_COPY_BATCH, JOIST_R_ALL, 0) == JOIST_OK && done == JOIST_COPY_BATCH);
    /* Per copy: one step and the account lookup (JOIST_MAX_BADGE_ACCTS). */
    JT_CHECK(k.work - w0 <= JOIST_COPY_BATCH * (1u + JOIST_MAX_BADGE_ACCTS));
    JT_CHECK(clean());
}

JT_TEST(batch_and_kill_together_hand_out_then_revoke)
{
    /* A trusted server's shape (mm itself never hands out frames: 0022's
     * amendment): the child gets the frame without the kill right, so it
     * can't revoke, and can't get the right back (I8); the giver can. */
    setup();
    JT_CHECK(batch(F, 1, (joist_rights)~JOIST_R_KILL, 0) == JOIST_OK && done == 1);
    JT_CHECK(joist_obj_kill(&k, C, F) == JOIST_E_RIGHTS);
    JT_CHECK(joist_cap_derive(&k, C, F, 9, JOIST_R_ALL) == JOIST_OK);
    JT_CHECK(joist_obj_kill(&k, C, 9) == JOIST_E_RIGHTS);
    JT_REQUIRE(joist_map(&k, root, F, 0, 1, TK, 0x400, JOIST_MAP_W) == JOIST_OK);
    JT_CHECK(joist_obj_kill(&k, root, F) == JOIST_OK);
    JT_CHECK(joist_map(&k, C, 9, 0, 1, 0, 0x600, 0) == JOIST_E_GONE);
    JT_CHECK(k.obj[k.cs[root].slot[F].obj - 1u].mapcount == 0); /* unmapped already */
    JT_CHECK(clean());
}

JT_TEST(kill_checker_sees_a_killed_frame_still_mapped)
{
    /* The state a kill that forgot to unmap would leave: the checker must
     * name it (made by hand, since the model never makes it). */
    setup();
    JT_REQUIRE(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_OK);
    JT_CHECK(clean());
    k.obj[k.cs[root].slot[F].obj - 1u].dead = 1; /* dead, mapped, and not counted for the sweep */
    char msg[200];
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0 && strstr(msg, "0022") != NULL);
}
