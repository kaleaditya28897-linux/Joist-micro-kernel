/* test_region.c - unit tests for regions (decision 0030; Phase 5 step 2):
 * a range of a task's addresses, whole last-level tables, inside which the
 * task maps pages of the region's extents itself. As test_map.c, the root
 * boots with a 16-page frame F, frames of simulated memory to lend, and a
 * task T (slot TK) with 32 lent pages. The root plays the memory manager: it
 * makes regions in T and derives T's capability (JOIST_R_MAP alone) into
 * slot RM of its own space, to use as T would. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

static struct joist_kernel k;
static joist_cspace root, T;

enum { TK = 0, F = 1, F2 = 2, RO = 3, MEM = 4, SELF = 5, MEM2 = 6, R = 7, RM = 8, X = 9 };

#define MEM_BASE 0x4000u
#define MEM_PAGES 64u
static uint64_t mem[MEM_PAGES][JOIST_PAGE_SIZE / 8u];

/* A region's first page: the start of a last-level table. */
#define LO ((uint64_t)8 * JOIST_PT_ENTRIES)

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

static joist_cspace space_of(joist_slot task_slot)
{
    return k.obj[k.cs[root].slot[task_slot].obj - 1u].target - 1u;
}

static uint32_t obj_of(joist_slot slot)
{
    return k.cs[root].slot[slot].obj - 1u;
}

static uint64_t pte(joist_cspace s, uint64_t va)
{
    uint64_t e = k.cs[s].pt_top[va >> JOIST_PT_SHIFT(3)];
    for (unsigned l = 2; e != 0 && l > 0; l--) {
        e = ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[(va >> JOIST_PT_SHIFT(l)) & (JOIST_PT_ENTRIES - 1u)];
    }
    return e == 0 ? 0 : ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[va & (JOIST_PT_ENTRIES - 1u)];
}

static void setup(void)
{
    joist_init(&k);
    memset(mem, 0xa5, sizeof mem);
    k.hooks.page = page;
    if (joist_boot(&k, 64, &root) != JOIST_OK || joist_boot_frame(&k, root, F, 0x100, 16) != JOIST_OK ||
        joist_boot_frame(&k, root, F2, 0x200, 8) != JOIST_OK ||
        joist_boot_frame(&k, root, MEM, MEM_BASE, 32) != JOIST_OK ||
        joist_boot_frame(&k, root, MEM2, MEM_BASE + 32u, 32) != JOIST_OK ||
        joist_boot_self(&k, root, SELF) != JOIST_OK || joist_task_create(&k, root, 20, TK) != JOIST_OK ||
        joist_space_donate(&k, root, TK, MEM) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    T = space_of(TK);
}

/* A two-table region over [LO, LO + 2 tables) of T allowing writing, with
 * F's pages [4, 12) as extent 0, and T's capability in RM. */
static void setup_region(void)
{
    setup();
    uint32_t x;
    if (joist_region_create(&k, root, TK, LO, 2, JOIST_MAP_W, R) != JOIST_OK ||
        joist_region_add(&k, root, R, F, 4, 8, &x) != JOIST_OK || x != 0 ||
        joist_cap_derive(&k, root, R, RM, JOIST_R_MAP) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's region could not be set up as written", 0);
    }
}

JT_TEST(region_create_and_its_refusals)
{
    setup();
    struct joist_cap c;
    JT_CHECK(joist_region_create(&k, root, TK, LO + 1u, 1, 0, R) == JOIST_E_RANGE); /* not on a table */
    JT_CHECK(joist_region_create(&k, root, TK, LO, 0, 0, R) == JOIST_E_RANGE);
    JT_CHECK(joist_region_create(&k, root, TK, LO, JOIST_REGION_TABLES + 1u, 0, R) == JOIST_E_RANGE);
    JT_CHECK(joist_region_create(&k, root, TK, JOIST_USER_PAGES - JOIST_PT_ENTRIES, 2, 0, R) ==
             JOIST_E_RANGE); /* past the user half */
    JT_CHECK(joist_region_create(&k, root, TK, LO, 1, 4u, R) == JOIST_E_RANGE); /* unknown ceiling bit */
    JT_CHECK(joist_region_create(&k, root, TK, LO, 1, 0, F) == JOIST_E_OCCUPIED);
    JT_CHECK(joist_region_create(&k, root, F, LO, 1, 0, R) == JOIST_E_TYPE); /* not a task */
    /* Something mapped there already: a table exists. */
    JT_REQUIRE(joist_map(&k, root, F, 0, 1, TK, LO + JOIST_PT_ENTRIES + 3u, 0) == JOIST_OK);
    JT_CHECK(joist_region_create(&k, root, TK, LO, 2, 0, R) == JOIST_E_STATE);
    JT_REQUIRE(joist_unmap_range(&k, root, TK, LO + JOIST_PT_ENTRIES + 3u, 1) == JOIST_OK);
    JT_CHECK(clean());
    uint32_t q = k.cs[root].quota;
    JT_REQUIRE(joist_region_create(&k, root, TK, LO, 2, JOIST_MAP_W, R) == JOIST_OK); /* gone: free again */
    JT_CHECK(k.cs[root].quota == q - 1u); /* paid by the maker */
    JT_REQUIRE(joist_cap_read(&k, root, R, &c) == JOIST_OK);
    JT_CHECK(c.type == JOIST_OBJ_REGION && c.rights == (JOIST_R_CONTROL | JOIST_R_MAP | JOIST_R_KILL | JOIST_R_GRANT));
    /* Regions of one task don't overlap; apart, they may touch. */
    JT_CHECK(joist_region_create(&k, root, TK, LO + JOIST_PT_ENTRIES, 1, 0, X) == JOIST_E_STATE);
    JT_CHECK(joist_region_create(&k, root, TK, LO - JOIST_PT_ENTRIES, 2, 0, X) == JOIST_E_STATE);
    JT_CHECK(joist_region_create(&k, root, TK, LO + 2u * JOIST_PT_ENTRIES, 1, 0, X) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(region_per_task_limit)
{
    setup();
    for (uint32_t i = 0; i < JOIST_REGIONS_PER_TASK; i++) {
        JT_REQUIRE(joist_region_create(&k, root, TK, LO + (uint64_t)i * JOIST_PT_ENTRIES, 1, 0, 16u + i) ==
                   JOIST_OK);
    }
    JT_CHECK(joist_region_create(&k, root, TK, LO + 64u * JOIST_PT_ENTRIES, 1, 0, X) == JOIST_E_NOMEM);
    JT_CHECK(clean());
}

JT_TEST(region_add_extents)
{
    setup();
    uint32_t x;
    JT_REQUIRE(joist_region_create(&k, root, TK, LO, 1, JOIST_MAP_W, R) == JOIST_OK);
    JT_CHECK(joist_region_add(&k, root, R, F, 0, 0, &x) == JOIST_E_RANGE);
    JT_CHECK(joist_region_add(&k, root, R, F, 10, 7, &x) == JOIST_E_RANGE); /* past the frame */
    JT_CHECK(joist_region_add(&k, root, R, MEM, 0, 1, &x) == JOIST_E_STATE); /* lent */
    JT_REQUIRE(joist_cap_derive(&k, root, R, RM, JOIST_R_MAP) == JOIST_OK);
    JT_CHECK(joist_region_add(&k, root, RM, F, 0, 1, &x) == JOIST_E_RIGHTS); /* the task can't */
    for (uint32_t i = 0; i < JOIST_REGION_EXTENTS; i++) {
        JT_REQUIRE(joist_region_add(&k, root, R, F, i, 1, &x) == JOIST_OK);
        JT_CHECK(x == i);
    }
    JT_CHECK(joist_region_add(&k, root, R, F2, 0, 1, &x) == JOIST_E_NOMEM);
    JT_CHECK(k.obj[obj_of(F)].extents == JOIST_REGION_EXTENTS);
    /* A frame in an extent can't be split or lent: the window would move,
     * or a task could write its own tables. */
    JT_CHECK(joist_frame_split(&k, root, F, 8, X) == JOIST_E_STATE);
    JT_CHECK(joist_space_donate(&k, root, TK, F) == JOIST_E_STATE);
    JT_CHECK(clean());
}

JT_TEST(region_map_by_the_task_inside_only)
{
    setup_region();
    /* The task maps pages of its extent anywhere in the range. */
    JT_CHECK(joist_region_map(&k, root, RM, 0, 0, 4, LO + 10u, JOIST_MAP_W) == JOIST_OK);
    JT_CHECK(joist_region_map(&k, root, RM, 0, 2, 3, LO + JOIST_PT_ENTRIES, 0) == JOIST_OK);
    JT_CHECK(JOIST_PTE_FRAME(pte(T, LO + 10u)) == obj_of(F) && JOIST_PTE_OFF(pte(T, LO + 10u)) == 4u);
    JT_CHECK(JOIST_PTE_OFF(pte(T, LO + JOIST_PT_ENTRIES + 2u)) == 8u);
    JT_CHECK(k.obj[obj_of(F)].mapcount == 7u);
    JT_CHECK(clean());
    /* Refusals. */
    JT_CHECK(joist_region_map(&k, root, RM, 1, 0, 1, LO, 0) == JOIST_E_RANGE);         /* no extent 1 */
    JT_CHECK(joist_region_map(&k, root, RM, 0, 6, 3, LO, 0) == JOIST_E_RANGE);         /* past it */
    JT_CHECK(joist_region_map(&k, root, RM, 0, 0, 1, LO - 1u, 0) == JOIST_E_RANGE);    /* before */
    JT_CHECK(joist_region_map(&k, root, RM, 0, 0, 1, LO + 2u * JOIST_PT_ENTRIES, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_region_map(&k, root, RM, 0, 0, 2, LO + JOIST_PT_ENTRIES - 1u, 0) ==
             JOIST_E_RANGE); /* across a table */
    JT_CHECK(joist_region_map(&k, root, RM, 0, 0, 1, LO, JOIST_MAP_X) == JOIST_E_RIGHTS); /* beyond the ceiling */
    JT_CHECK(joist_region_map(&k, root, RM, 0, 0, 1, LO, JOIST_MAP_W | JOIST_MAP_X) == JOIST_E_RIGHTS);
    JT_CHECK(joist_region_map(&k, root, RM, 0, 0, 1, LO + 11u, 0) == JOIST_E_OCCUPIED);
    JT_CHECK(joist_region_map(&k, root, R, 0, 0, 1, LO, 0) == JOIST_OK); /* the maker's has MAP too */
    /* Plain calls can't touch the range: only region calls act there. */
    JT_CHECK(joist_map(&k, root, F2, 0, 1, TK, LO + 100u, 0) == JOIST_E_STATE);
    /* (A run can't half-overlap a region: runs stay inside one table, and
     * regions cover whole tables. One across the edge is refused as any
     * run across a table is.) */
    JT_CHECK(joist_map(&k, root, F2, 0, 2, TK, LO - 1u, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_unmap_range(&k, root, TK, LO + 10u, 1) == JOIST_E_STATE);
    JT_CHECK(joist_protect(&k, root, TK, LO + 10u, 1, 0) == JOIST_E_STATE);
    JT_CHECK(joist_map_replace(&k, root, F2, 0, 1, TK, LO + 10u, 0, F, 4) == JOIST_E_STATE);
    JT_CHECK(joist_map(&k, root, F2, 0, 1, TK, LO - 1u, 0) == JOIST_OK); /* just outside */
    JT_CHECK(clean());
}

JT_TEST(region_map_writable_only_from_a_writable_frame)
{
    setup();
    uint32_t x;
    JT_REQUIRE(joist_cap_derive(&k, root, F2, RO, (joist_rights)(JOIST_R_CONTROL | JOIST_R_GRANT)) == JOIST_OK);
    JT_REQUIRE(joist_region_create(&k, root, TK, LO, 1, JOIST_MAP_W, R) == JOIST_OK);
    JT_REQUIRE(joist_region_add(&k, root, R, RO, 0, 4, &x) == JOIST_OK);
    JT_CHECK(joist_region_map(&k, root, R, x, 0, 1, LO, JOIST_MAP_W) == JOIST_E_RIGHTS);
    JT_REQUIRE(joist_region_map(&k, root, R, x, 0, 1, LO, 0) == JOIST_OK);
    JT_CHECK(joist_region_protect(&k, root, R, LO, 1, JOIST_MAP_W) == JOIST_E_RIGHTS);
    JT_CHECK(clean());
}

JT_TEST(region_unmap_and_protect_inside)
{
    setup_region();
    JT_REQUIRE(joist_region_map(&k, root, RM, 0, 0, 4, LO + 10u, JOIST_MAP_W) == JOIST_OK);
    JT_CHECK(joist_region_protect(&k, root, RM, LO + 10u, 4, 0) == JOIST_OK);
    JT_CHECK(JOIST_PTE_PERMS(pte(T, LO + 11u)) == 0);
    JT_CHECK(joist_region_protect(&k, root, RM, LO + 10u, 4, JOIST_MAP_X) == JOIST_E_RIGHTS);
    JT_CHECK(joist_region_protect(&k, root, RM, LO - 1u, 2, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_region_unmap(&k, root, RM, LO + 2u * JOIST_PT_ENTRIES, 1) == JOIST_E_RANGE);
    JT_CHECK(joist_region_unmap(&k, root, RM, LO + 10u, 4) == JOIST_OK);
    JT_CHECK(pte(T, LO + 10u) == 0 && k.obj[obj_of(F)].mapcount == 0 && k.cs[T].pt_used == 0);
    JT_CHECK(clean()); /* the emptied table is given back */
}

JT_TEST(region_kill_unmaps_its_range_on_the_work_list)
{
    setup_region();
    JT_REQUIRE(joist_region_map(&k, root, RM, 0, 0, 4, LO + 10u, JOIST_MAP_W) == JOIST_OK);
    JT_REQUIRE(joist_region_map(&k, root, RM, 0, 4, 4, LO + JOIST_PT_ENTRIES, 0) == JOIST_OK);
    /* F mapped outside the region too (as mm maps its pool): left alone. */
    JT_REQUIRE(joist_space_donate(&k, root, SELF, MEM2) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 0, 2, SELF, 0x400, 0) == JOIST_OK);
    uint32_t ro = obj_of(R);
    k.preempt = 1;
    JT_REQUIRE(joist_obj_kill(&k, root, R) == JOIST_OK);
    JT_CHECK(k.regions_dying == 1);
    JT_CHECK(joist_region_map(&k, root, RM, 0, 0, 1, LO, 0) == JOIST_E_GONE);
    JT_CHECK(joist_map(&k, root, F2, 0, 1, TK, LO + 100u, 0) == JOIST_E_STATE); /* still its range */
    JT_CHECK(clean());
    unsigned steps = 0;
    while (joist_continue(&k, 1) == JOIST_BLOCKED && steps < 1000u) {
        steps++;
        JT_CHECK(clean());
    }
    k.preempt = 0;
    JT_CHECK(k.regions_dying == 0);
    JT_CHECK(pte(T, LO + 10u) == 0 && pte(T, LO + JOIST_PT_ENTRIES) == 0 && k.cs[T].pt_used == 0);
    JT_CHECK(k.obj[obj_of(F)].mapcount == 2u && k.obj[obj_of(F)].extents == 0);
    JT_CHECK(k.obj[ro].type == JOIST_OBJ_REGION && k.obj[ro].dead); /* its capabilities still held */
    JT_CHECK(joist_region_unmap(&k, root, R, LO, 1) == JOIST_E_GONE);
    JT_CHECK(joist_obj_kill(&k, root, R) == JOIST_E_GONE);
    /* Its range is free again for plain calls, and for a new region. */
    JT_CHECK(joist_map(&k, root, F2, 0, 1, TK, LO + 100u, 0) == JOIST_OK);
    JT_REQUIRE(joist_unmap_range(&k, root, TK, LO + 100u, 1) == JOIST_OK);
    JT_CHECK(joist_region_create(&k, root, TK, LO, 2, 0, X) == JOIST_OK);
    /* The record goes with the last capability. */
    uint32_t live = 0;
    JT_REQUIRE(joist_cap_delete(&k, root, R) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, root, RM) == JOIST_OK);
    for (uint32_t i = 0; i < JOIST_MAX_REGIONS; i++) {
        live += k.reg[i].live;
    }
    JT_CHECK(live == 1 && k.obj[ro].type != JOIST_OBJ_REGION);
    JT_CHECK(clean());
}

JT_TEST(region_last_capability_gone_ends_it)
{
    setup_region();
    JT_REQUIRE(joist_region_map(&k, root, RM, 0, 0, 4, LO + 10u, JOIST_MAP_W) == JOIST_OK);
    uint32_t q = k.cs[root].quota;
    JT_REQUIRE(joist_cap_delete(&k, root, RM) == JOIST_OK);
    JT_CHECK(pte(T, LO + 10u) != 0); /* a capability remains */
    JT_REQUIRE(joist_cap_delete(&k, root, R) == JOIST_OK);
    JT_CHECK(pte(T, LO + 10u) == 0 && k.regions_dying == 0 && k.cs[root].quota == q + 1u);
    JT_CHECK(k.obj[obj_of(F)].mapcount == 0 && k.obj[obj_of(F)].extents == 0);
    JT_CHECK(k.cs[T].region[0] == 0 && !k.reg[0].live);
    JT_CHECK(clean());
}

JT_TEST(region_dying_frame_kept_by_an_extent)
{
    setup_region();
    uint32_t fo = obj_of(F);
    JT_REQUIRE(joist_cap_delete(&k, root, F) == JOIST_OK); /* F's last capability */
    JT_CHECK(k.obj[fo].type == JOIST_OBJ_FRAME && k.obj[fo].dying); /* kept by the extent */
    JT_CHECK(joist_region_map(&k, root, RM, 0, 0, 1, LO, 0) == JOIST_E_GONE);
    JT_CHECK(clean());
    JT_REQUIRE(joist_obj_kill(&k, root, R) == JOIST_OK);
    JT_CHECK(k.obj[fo].type == JOIST_OBJ_NONE); /* freed once the extent goes */
    JT_CHECK(clean());
}

JT_TEST(region_frame_killed_while_mapped_in_it)
{
    setup_region();
    uint32_t x;
    JT_REQUIRE(joist_region_add(&k, root, R, F2, 0, 8, &x) == JOIST_OK && x == 1);
    JT_REQUIRE(joist_region_map(&k, root, RM, 0, 0, 2, LO, 0) == JOIST_OK);
    JT_REQUIRE(joist_region_map(&k, root, RM, 1, 0, 2, LO + 2u, 0) == JOIST_OK);
    JT_REQUIRE(joist_obj_kill(&k, root, F) == JOIST_OK);
    JT_CHECK(pte(T, LO) == 0 && pte(T, LO + 2u) != 0); /* F's pages gone, F2's stay */
    JT_CHECK(joist_region_map(&k, root, RM, 0, 2, 1, LO + 5u, 0) == JOIST_E_GONE);
    JT_CHECK(joist_region_map(&k, root, RM, 1, 2, 1, LO + 5u, 0) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(region_task_destroyed)
{
    setup_region();
    JT_REQUIRE(joist_region_map(&k, root, RM, 0, 0, 4, LO + 10u, JOIST_MAP_W) == JOIST_OK);
    uint32_t ro = obj_of(R);
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK);
    JT_CHECK(!k.cs[T].live && k.obj[ro].dead);
    JT_CHECK(k.obj[obj_of(F)].mapcount == 0 && k.obj[obj_of(F)].extents == 0);
    JT_CHECK(joist_region_map(&k, root, R, 0, 0, 1, LO, 0) == JOIST_E_GONE);
    JT_CHECK(joist_region_add(&k, root, R, F2, 0, 1, &(uint32_t){0}) == JOIST_E_GONE);
    JT_CHECK(clean());
    JT_REQUIRE(joist_cap_delete(&k, root, RM) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, root, R) == JOIST_OK);
    JT_CHECK(!k.reg[0].live && k.obj[ro].type == JOIST_OBJ_NONE);
    JT_CHECK(clean());
}

JT_TEST(region_capability_not_passed_on_by_the_task)
{
    setup_region();
    struct joist_cap c;
    JT_REQUIRE(joist_cap_read(&k, root, RM, &c) == JOIST_OK);
    JT_CHECK(c.rights == JOIST_R_MAP); /* no GRANT, CONTROL or KILL */
    JT_CHECK(joist_cap_give(&k, root, TK, RM, 3, JOIST_R_MAP) == JOIST_E_RIGHTS);
    JT_CHECK(joist_obj_kill(&k, root, RM) == JOIST_E_RIGHTS);
    JT_CHECK(joist_cap_give(&k, root, TK, R, 3, JOIST_R_MAP) == JOIST_OK); /* the maker can */
    JT_CHECK(clean());
}

JT_TEST(region_invariants_catch_bad_state)
{
    char msg[200];
    setup_region();
    JT_REQUIRE(joist_region_map(&k, root, RM, 0, 0, 2, LO, 0) == JOIST_OK);
    JT_REQUIRE(clean());
    /* An entry in the range naming a page outside its extents. */
    uint64_t *lf = NULL;
    {
        uint64_t e = k.cs[T].pt_top[LO >> JOIST_PT_SHIFT(3)];
        for (unsigned l = 2; e != 0 && l > 0; l--) {
            e = ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[(LO >> JOIST_PT_SHIFT(l)) & (JOIST_PT_ENTRIES - 1u)];
        }
        lf = (uint64_t *)page(NULL, JOIST_PT_SHADOW(e));
    }
    uint64_t was = lf[LO % JOIST_PT_ENTRIES];
    lf[LO % JOIST_PT_ENTRIES] = JOIST_PTE(obj_of(F), 0u, 0u, 1); /* page 0 isn't in [4, 12) */
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0 && strstr(msg, "no page of its extents") != NULL);
    lf[LO % JOIST_PT_ENTRIES] = was;
    JT_CHECK(clean());
    /* A frame's extents count off by one. */
    k.obj[obj_of(F)].extents++;
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0 && strstr(msg, "extents") != NULL);
    k.obj[obj_of(F)].extents--;
    /* An empty last-level table left behind. */
    lf[LO % JOIST_PT_ENTRIES] = 0;
    lf[(LO + 1u) % JOIST_PT_ENTRIES] = 0;
    k.obj[obj_of(F)].mapcount -= 2u;
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0 && strstr(msg, "empty last-level table") != NULL);
}
