/* test_map.c - unit tests for frames, page tables and mappings (Phase 3
 * step 3, Phase 4 step 2; 0013, 0020, 0021; invariant I12). The root boots
 * with a 16-page frame in slot F, a frame of simulated memory to lend for
 * page tables in slot MEM, a capability to itself in slot SELF, and makes a
 * task T (slot TK) to map into. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

static struct joist_kernel k;
static joist_cspace root, T;

enum { TK = 0, F = 1, F2 = 2, RO = 3, MEM = 4, SELF = 5, MEM2 = 6, X = 9 }; /* root's slots */

/* Simulated physical memory for the pages lent to spaces: [MEM_BASE,
 * MEM_BASE + MEM_PAGES). */
#define MEM_BASE 0x4000u
#define MEM_PAGES 64u
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

static joist_cspace space_of(joist_slot task_slot)
{
    return k.obj[k.cs[root].slot[task_slot].obj - 1u].target - 1u;
}

static const struct joist_object *frame(joist_slot slot)
{
    return &k.obj[k.cs[root].slot[slot].obj - 1u];
}

/* The world, with nothing lent yet. */
static void setup_bare(void)
{
    joist_init(&k);
    memset(mem, 0xa5, sizeof mem); /* a page taken for a table must be zeroed */
    k.hooks.page = page;
    if (joist_boot(&k, 64, &root) != JOIST_OK || joist_boot_frame(&k, root, F, 0x100, 16) != JOIST_OK ||
        joist_boot_frame(&k, root, MEM, MEM_BASE, 32) != JOIST_OK ||
        joist_boot_frame(&k, root, MEM2, MEM_BASE + 32u, 32) != JOIST_OK ||
        joist_boot_self(&k, root, SELF) != JOIST_OK || joist_task_create(&k, root, 20, TK) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    T = space_of(TK);
}

/* ...and 32 pages lent to T. */
static void setup(void)
{
    setup_bare();
    if (joist_space_donate(&k, root, TK, MEM) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
    }
}

/* The entry recorded for virtual page va of space s, 0 if none. */
static uint64_t pte(joist_cspace s, uint64_t va)
{
    uint64_t e = k.cs[s].pt_top[va >> JOIST_PT_SHIFT(3)];
    for (unsigned l = 2; e != 0 && l > 0; l--) {
        e = ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[(va >> JOIST_PT_SHIFT(l)) & (JOIST_PT_ENTRIES - 1u)];
    }
    return e == 0 ? 0 : ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[va & (JOIST_PT_ENTRIES - 1u)];
}

JT_TEST(map_boot_frames)
{
    setup_bare();
    struct joist_cap c;
    JT_REQUIRE(joist_cap_read(&k, root, F, &c) == JOIST_OK);
    JT_CHECK(c.type == JOIST_OBJ_FRAME &&
             c.rights == (JOIST_R_CONTROL | JOIST_R_WRITE | JOIST_R_KILL | JOIST_R_GRANT));
    JT_CHECK(joist_boot_frame(&k, root, F2, 0x10f, 4) == JOIST_E_STATE); /* overlaps the last page */
    JT_CHECK(joist_boot_frame(&k, root, F2, 0x50, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_boot_frame(&k, root, F2, UINT64_MAX, 2) == JOIST_E_RANGE); /* wraps */
    JT_CHECK(joist_boot_frame(&k, root, F2, 0x1000000, JOIST_FRAME_PAGES_MAX + 1u) == JOIST_E_RANGE);
    JT_CHECK(joist_boot_frame(&k, root, F, 0x200, 1) == JOIST_E_OCCUPIED);
    JT_CHECK(joist_boot_frame(&k, T, 0, 0x200, 1) == JOIST_E_STATE); /* only the root */
    JT_CHECK(joist_boot_frame(&k, root, F2, 0x110, 4) == JOIST_OK);  /* adjacent is fine */
    JT_CHECK(clean());
}

JT_TEST(map_check_finds_an_overlap_between_frames_far_apart)
{
    /* I12's check sorts the frames by their first pages (O(n log n)): an
     * overlap between frames far apart in the table, and in base order, is
     * still found, and frames out of base order that don't overlap are not
     * one. */
    setup_bare();
    JT_REQUIRE(joist_boot_frame(&k, root, F2, 0x3000, 4) == JOIST_OK);
    JT_REQUIRE(joist_boot_frame(&k, root, RO, 0x2000, 4) == JOIST_OK);
    JT_REQUIRE(joist_boot_frame(&k, root, X, 0x1000, 4) == JOIST_OK);
    JT_CHECK(clean()); /* made in falling base order: none overlaps */
    struct joist_object *last = &k.obj[k.cs[root].slot[X].obj - 1u];
    last->base = 0x10e; /* now over F's last two pages, F made first */
    char msg[200];
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0 && strstr(msg, "overlap") != NULL);
    last->base = 0x1000;
    JT_CHECK(clean());
}

JT_TEST(map_boot_self)
{
    setup_bare();
    struct joist_cap c;
    JT_REQUIRE(joist_cap_read(&k, root, SELF, &c) == JOIST_OK);
    JT_CHECK(c.type == JOIST_OBJ_TASK && (c.rights & JOIST_R_CONTROL) && space_of(SELF) == root);
    JT_CHECK(joist_boot_self(&k, root, X) == JOIST_E_STATE); /* one only */
    JT_CHECK(joist_boot_self(&k, T, 0) == JOIST_E_STATE);    /* the root only */
    /* Its last capability gone, the root is not destroyed (0013). */
    JT_REQUIRE(joist_cap_delete(&k, root, SELF) == JOIST_OK);
    JT_CHECK(k.cs[root].live && !k.cs[root].dying && k.cs[root].task_obj == 0);
    JT_CHECK(joist_boot_self(&k, root, SELF) == JOIST_OK); /* and it may have one again */
    JT_CHECK(clean());
}

JT_TEST(map_split)
{
    setup_bare();
    JT_CHECK(joist_frame_split(&k, root, F, 0, F2) == JOIST_E_RANGE);
    JT_CHECK(joist_frame_split(&k, root, F, 16, F2) == JOIST_E_RANGE);
    JT_CHECK(joist_frame_split(&k, root, F, 4, TK) == JOIST_E_OCCUPIED);
    JT_REQUIRE(joist_frame_split(&k, root, F, 4, F2) == JOIST_OK);
    JT_CHECK(frame(F)->base == 0x100 && frame(F)->npages == 4 && frame(F2)->base == 0x104 &&
             frame(F2)->npages == 12);
    JT_CHECK(k.cs[root].slot[F2].rights == k.cs[root].slot[F].rights);
    /* A shared, mapped or lent frame isn't split. */
    JT_REQUIRE(joist_cap_derive(&k, root, F2, RO, JOIST_R_CONTROL) == JOIST_OK);
    JT_CHECK(joist_frame_split(&k, root, F2, 2, X) == JOIST_E_STATE);
    JT_REQUIRE(joist_space_donate(&k, root, TK, MEM) == JOIST_OK);
    JT_CHECK(joist_frame_split(&k, root, MEM, 2, X) == JOIST_E_STATE);
    JT_REQUIRE(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_OK);
    JT_CHECK(joist_frame_split(&k, root, F, 2, X) == JOIST_E_STATE);
    JT_CHECK(clean());
}

JT_TEST(map_needs_lent_pages)
{
    setup_bare();
    JT_CHECK(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_E_NOMEM); /* nothing lent */
    JT_CHECK(frame(F)->mapcount == 0 && k.cs[T].pt_top[0] == 0);
    /* Lending: CONTROL on both; never a mapped, lent or unreachable frame. */
    JT_REQUIRE(joist_cap_derive(&k, root, MEM2, RO, JOIST_R_GRANT) == JOIST_OK);
    JT_CHECK(joist_space_donate(&k, root, TK, RO) == JOIST_E_RIGHTS);
    JT_CHECK(joist_space_donate(&k, root, MEM, TK) == JOIST_E_TYPE);
    JT_CHECK(joist_space_donate(&k, root, TK, F) == JOIST_E_STATE); /* outside the reachable memory */
    JT_REQUIRE(joist_space_donate(&k, root, TK, MEM) == JOIST_OK);
    JT_CHECK(frame(MEM)->lent == T + 1u && k.cs[T].pt_avail == 32);
    JT_CHECK(joist_space_donate(&k, root, SELF, MEM) == JOIST_E_STATE); /* lent already */
    JT_CHECK(joist_map(&k, root, MEM, 0, 1, TK, 0x800, 0) == JOIST_E_STATE); /* a lent frame */
    /* The first run in a new place takes three tables, two pages each. */
    JT_REQUIRE(joist_map(&k, root, F, 0, 4, TK, 0x400, JOIST_MAP_W) == JOIST_OK);
    JT_CHECK(k.cs[T].pt_used == 6 && k.cs[T].pt_avail == 26 && frame(F)->mapcount == 4);
    /* The same last-level table: nothing more. A new one under the same
     * level-1 table: two pages. A new top-level entry: six. */
    JT_REQUIRE(joist_map(&k, root, F, 4, 2, TK, 0x410, 0) == JOIST_OK);
    JT_CHECK(k.cs[T].pt_used == 6);
    JT_REQUIRE(joist_map(&k, root, F, 6, 1, TK, 0x600, 0) == JOIST_OK);
    JT_CHECK(k.cs[T].pt_used == 8);
    JT_REQUIRE(joist_map(&k, root, F, 7, 1, TK, (uint64_t)1 << JOIST_PT_SHIFT(3), 0) == JOIST_OK);
    JT_CHECK(k.cs[T].pt_used == 14 && k.cs[T].pt_avail == 18);
    JT_CHECK(clean());
}

JT_TEST(map_needs_exactly_the_pages_its_tables_take)
{
    /* Five pages lent where the first run takes six: refused, nothing
     * changes; one more, and it fits with none left. */
    setup_bare();
    JT_REQUIRE(joist_frame_split(&k, root, MEM, 5, X) == JOIST_OK);
    JT_REQUIRE(joist_space_donate(&k, root, TK, MEM) == JOIST_OK);
    JT_CHECK(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_E_NOMEM);
    JT_CHECK(k.cs[T].pt_used == 0 && k.cs[T].pt_avail == 5 && frame(F)->mapcount == 0);
    JT_REQUIRE(joist_frame_split(&k, root, X, 1, F2) == JOIST_OK);
    JT_REQUIRE(joist_space_donate(&k, root, TK, X) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_OK);
    JT_CHECK(k.cs[T].pt_used == 6 && k.cs[T].pt_avail == 0);
    /* A new last-level table now: two pages, and none are left. */
    JT_CHECK(joist_map(&k, root, F, 1, 1, TK, 0x600, 0) == JOIST_E_NOMEM);
    JT_CHECK(joist_map(&k, root, F, 1, 1, TK, 0x401, 0) == JOIST_OK); /* the same table: none */
    JT_CHECK(clean());
}

JT_TEST(map_lends_only_reachable_pages)
{
    /* The core keeps its tables in lent pages, so every page of a lent
     * frame must be reachable: its first and its last are checked. */
    setup_bare();
    JT_REQUIRE(joist_cap_delete(&k, root, MEM) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, root, MEM2) == JOIST_OK);
    JT_REQUIRE(joist_boot_frame(&k, root, MEM, MEM_BASE - 4u, 8) == JOIST_OK); /* first page not */
    JT_CHECK(joist_space_donate(&k, root, TK, MEM) == JOIST_E_STATE);
    JT_REQUIRE(joist_boot_frame(&k, root, MEM2, MEM_BASE + MEM_PAGES - 4u, 8) == JOIST_OK); /* last not */
    JT_CHECK(joist_space_donate(&k, root, TK, MEM2) == JOIST_E_STATE);
    JT_REQUIRE(joist_boot_frame(&k, root, X, MEM_BASE + 8u, 8) == JOIST_OK);
    JT_CHECK(joist_space_donate(&k, root, TK, X) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(map_runs_of_a_frame)
{
    setup();
    JT_REQUIRE(joist_map(&k, root, F, 3, 5, TK, 0x402, JOIST_MAP_W) == JOIST_OK);
    for (uint64_t i = 0; i < 5; i++) {
        uint64_t e = pte(T, 0x402 + i);
        JT_CHECK(JOIST_PTE_FRAME(e) == k.cs[root].slot[F].obj - 1u && JOIST_PTE_OFF(e) == 3 + i &&
                 JOIST_PTE_PERMS(e) == JOIST_MAP_W && (e & JOIST_PTE_CEIL_W));
    }
    JT_CHECK(pte(T, 0x407) == 0 && pte(T, 0x401) == 0);
    JT_CHECK(joist_map(&k, root, F, 0, 1, TK, 0x406, 0) == JOIST_E_OCCUPIED); /* its last page */
    JT_CHECK(joist_map(&k, root, F, 0, 2, TK, 0x401, 0) == JOIST_E_OCCUPIED); /* its first */
    JT_REQUIRE(joist_map(&k, root, F, 3, 5, TK, 0x407, JOIST_MAP_X) == JOIST_OK); /* aliasing is allowed */
    JT_CHECK(frame(F)->mapcount == 10);
    JT_CHECK(clean());
}

JT_TEST(map_refusals)
{
    setup();
    uint64_t used = k.cs[T].pt_used;
    JT_CHECK(joist_map(&k, root, F, 0, 1, TK, 0x400, JOIST_MAP_W | JOIST_MAP_X) == JOIST_E_RIGHTS);
    JT_CHECK(joist_map(&k, root, F, 0, 1, TK, 0x400, 4) == JOIST_E_RIGHTS); /* unknown permission */
    JT_REQUIRE(joist_cap_derive(&k, root, F, RO, JOIST_R_CONTROL) == JOIST_OK); /* no WRITE */
    JT_CHECK(joist_map(&k, root, RO, 0, 1, TK, 0x400, JOIST_MAP_W) == JOIST_E_RIGHTS);
    JT_CHECK(joist_map(&k, root, F, 0, 0, TK, 0x400, 0) == JOIST_E_RANGE);  /* nothing */
    JT_CHECK(joist_map(&k, root, F, 16, 1, TK, 0x400, 0) == JOIST_E_RANGE); /* past the frame */
    JT_CHECK(joist_map(&k, root, F, 8, 9, TK, 0x400, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_map(&k, root, F, 0, 9, TK, JOIST_USER_PAGES - 8u, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_map(&k, root, F, 0, 1, TK, JOIST_USER_PAGES, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_map(&k, root, F, 0, 9, TK, 0x600 - 8u, 0) == JOIST_E_RANGE); /* crosses 0x600 */
    JT_CHECK(joist_map(&k, root, F, 0, 1, F2, 0x800, 0) == JOIST_E_EMPTY);
    JT_CHECK(joist_map(&k, root, TK, 0, 1, TK, 0x800, 0) == JOIST_E_TYPE);
    JT_CHECK(joist_map(&k, root, F, 0, 1, JOIST_SLOT_SELF, 0x800, 0) == JOIST_E_RANGE); /* no SELF */
    JT_CHECK(k.cs[T].pt_used == used && frame(F)->mapcount == 0); /* none changed anything */
    JT_CHECK(joist_map(&k, root, RO, 0, 1, TK, 0x400, 0) == JOIST_OK); /* read-only is fine */
    JT_CHECK(joist_map(&k, root, F, 0, 8, TK, 0x600 - 8u, 0) == JOIST_OK); /* ends exactly there */
    JT_CHECK(joist_map(&k, root, F, 8, 8, TK, JOIST_USER_PAGES - 8u, 0) == JOIST_OK); /* the top */
    JT_CHECK(clean());
}

JT_TEST(map_replace_only_what_is_expected)
{
    setup();
    JT_REQUIRE(joist_boot_frame(&k, root, F2, 0x200, 4) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 2, 3, TK, 0x400, 0) == JOIST_OK);
    /* Anything but exactly F's pages 2-4 there: refused, nothing changes. */
    JT_CHECK(joist_map_replace(&k, root, F2, 0, 3, TK, 0x400, JOIST_MAP_W, F, 1) == JOIST_E_STATE);
    JT_CHECK(joist_map_replace(&k, root, F2, 0, 3, TK, 0x400, JOIST_MAP_W, F2, 2) == JOIST_E_STATE);
    JT_CHECK(joist_map_replace(&k, root, F2, 0, 4, TK, 0x400, JOIST_MAP_W, F, 2) == JOIST_E_STATE); /* a page absent */
    JT_CHECK(joist_map_replace(&k, root, F2, 0, 1, TK, 0x800, 0, F, 2) == JOIST_E_STATE); /* no table */
    JT_CHECK(frame(F)->mapcount == 3 && frame(F2)->mapcount == 0 && JOIST_PTE_OFF(pte(T, 0x400)) == 2);
    /* As expected: replaced. */
    JT_REQUIRE(joist_map_replace(&k, root, F2, 1, 2, TK, 0x401, JOIST_MAP_W, F, 3) == JOIST_OK);
    JT_CHECK(frame(F)->mapcount == 1 && frame(F2)->mapcount == 2);
    JT_CHECK(JOIST_PTE_FRAME(pte(T, 0x401)) == k.cs[root].slot[F2].obj - 1u &&
             JOIST_PTE_OFF(pte(T, 0x402)) == 2 && JOIST_PTE_PERMS(pte(T, 0x402)) == JOIST_MAP_W);
    /* Its own rights hold for the new frame. */
    JT_REQUIRE(joist_cap_derive(&k, root, F2, RO, JOIST_R_CONTROL) == JOIST_OK);
    JT_CHECK(joist_map_replace(&k, root, RO, 0, 1, TK, 0x400, JOIST_MAP_W, F, 2) == JOIST_E_RIGHTS);
    JT_CHECK(clean());
}

JT_TEST(map_unmap_range_with_holes_gives_tables_back)
{
    setup();
    uint64_t avail = k.cs[T].pt_avail;
    JT_REQUIRE(joist_map(&k, root, F, 0, 2, TK, 0x400, 0) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 4, 2, TK, 0x408, 0) == JOIST_OK);
    JT_CHECK(joist_unmap_range(&k, root, TK, 0x400, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_unmap_range(&k, root, TK, 0x5ff, 2) == JOIST_E_RANGE); /* crosses a table */
    JT_REQUIRE(joist_unmap_range(&k, root, TK, 0x401, 8) == JOIST_OK);   /* holes in between */
    JT_CHECK(pte(T, 0x400) != 0 && pte(T, 0x401) == 0 && pte(T, 0x408) == 0 && pte(T, 0x409) != 0);
    JT_CHECK(frame(F)->mapcount == 2 && k.cs[T].pt_used == 6);
    JT_REQUIRE(joist_unmap_range(&k, root, TK, 0x400, 512) == JOIST_OK); /* the rest: tables go */
    JT_CHECK(frame(F)->mapcount == 0 && k.cs[T].pt_used == 0 && k.cs[T].pt_avail == avail);
    JT_CHECK(k.cs[T].pt_top[0] == 0);
    JT_REQUIRE(joist_unmap_range(&k, root, TK, 0x400, 4) == JOIST_OK); /* nothing there: fine */
    /* The pages given back are used again. */
    JT_REQUIRE(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_OK);
    JT_CHECK(k.cs[T].pt_used == 6 && k.cs[T].pt_avail == avail - 6u);
    JT_CHECK(clean());
}

JT_TEST(map_query_reports_pages_and_the_named_frame_only)
{
    /* 0020, amended: per page, absent, or present with its permissions and
     * ceiling, and the page offset only of the frame the caller names. */
    setup();
    JT_REQUIRE(joist_map(&k, root, F, 2, 3, TK, 0x401, JOIST_MAP_W) == JOIST_OK);
    JT_REQUIRE(joist_boot_frame(&k, root, F2, 0x900, 4) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F2, 1, 1, TK, 0x404, JOIST_MAP_X) == JOIST_OK);
    uint64_t q[6];
    const uint64_t pw = JOIST_QUERY_PRESENT | JOIST_MAP_W | JOIST_QUERY_CEIL_W;
    const uint64_t px = JOIST_QUERY_PRESENT | JOIST_MAP_X | JOIST_QUERY_CEIL_W;
    JT_REQUIRE(joist_map_query(&k, root, TK, 0x400, 6, F, q) == JOIST_OK);
    JT_CHECK(q[0] == 0 && q[5] == 0);
    for (unsigned i = 0; i < 3; i++) {
        JT_CHECK(q[1 + i] == (pw | JOIST_QUERY_FRAME | ((uint64_t)(2 + i) << 32)));
        JT_CHECK(JOIST_QUERY_OFF(q[1 + i]) == 2u + i);
    }
    JT_CHECK(q[4] == px); /* another frame: present, never which */
    JT_REQUIRE(joist_map_query(&k, root, TK, 0x400, 6, JOIST_SLOT_NONE, q) == JOIST_OK);
    JT_CHECK(q[1] == pw && q[3] == pw && q[4] == px);
    JT_REQUIRE(joist_map_query(&k, root, TK, 0x400, 6, F2, q) == JOIST_OK);
    JT_CHECK(q[1] == pw && q[4] == (px | JOIST_QUERY_FRAME | ((uint64_t)1 << 32)));
    /* A read-only-ceiling mapping reports no ceiling. */
    JT_REQUIRE(joist_cap_derive(&k, root, F, RO, JOIST_R_CONTROL) == JOIST_OK); /* no WRITE */
    JT_REQUIRE(joist_map(&k, root, RO, 9, 1, TK, 0x410, 0) == JOIST_OK);
    JT_REQUIRE(joist_map_query(&k, root, TK, 0x410, 1, F, q) == JOIST_OK);
    JT_CHECK(q[0] == (JOIST_QUERY_PRESENT | JOIST_QUERY_FRAME | ((uint64_t)9 << 32)));
    /* Errors change nothing, the buffer included. */
    for (unsigned i = 0; i < 6; i++) {
        q[i] = 0x5a5a5a5a5a5a5a5aull;
    }
    JT_CHECK(joist_map_query(&k, root, TK, 0x400, 0, F, q) == JOIST_E_RANGE);
    JT_CHECK(joist_map_query(&k, root, TK, 0x400, JOIST_QUERY_MAX + 1u, F, q) == JOIST_E_RANGE);
    JT_CHECK(joist_map_query(&k, root, TK, JOIST_MAP_SPAN - 1u, 2, F, q) == JOIST_E_RANGE);
    JT_CHECK(joist_map_query(&k, root, TK, JOIST_USER_PAGES, 1, F, q) == JOIST_E_RANGE);
    JT_CHECK(joist_map_query(&k, root, F, 0x400, 1, F, q) == JOIST_E_TYPE);  /* not a task */
    JT_CHECK(joist_map_query(&k, root, TK, 0x400, 1, TK, q) == JOIST_E_TYPE); /* not a frame */
    JT_REQUIRE(joist_cap_derive(&k, root, F, X, JOIST_R_WRITE) == JOIST_OK); /* no CONTROL */
    JT_CHECK(joist_map_query(&k, root, TK, 0x400, 1, X, q) == JOIST_E_RIGHTS);
    for (unsigned i = 0; i < 6; i++) {
        JT_CHECK(q[i] == 0x5a5a5a5a5a5a5a5aull);
    }
    JT_CHECK(clean());
}

JT_TEST(map_protect_up_to_the_ceiling)
{
    setup();
    JT_REQUIRE(joist_cap_derive(&k, root, F, RO, JOIST_R_CONTROL) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 0, 2, TK, 0x400, JOIST_MAP_W) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, RO, 2, 1, TK, 0x402, 0) == JOIST_OK);
    JT_CHECK(joist_protect(&k, root, TK, 0x400, 4, JOIST_MAP_W | JOIST_MAP_X) == JOIST_E_RIGHTS);
    JT_CHECK(joist_protect(&k, root, TK, 0x400, 4, JOIST_MAP_W) == JOIST_E_RIGHTS); /* 0x402's ceiling */
    JT_CHECK(JOIST_PTE_PERMS(pte(T, 0x400)) == JOIST_MAP_W && JOIST_PTE_PERMS(pte(T, 0x402)) == 0);
    JT_REQUIRE(joist_protect(&k, root, TK, 0x400, 4, 0) == JOIST_OK); /* down, holes and all */
    JT_CHECK(JOIST_PTE_PERMS(pte(T, 0x400)) == 0 && JOIST_PTE_PERMS(pte(T, 0x401)) == 0);
    JT_REQUIRE(joist_protect(&k, root, TK, 0x400, 2, JOIST_MAP_W) == JOIST_OK); /* and back */
    JT_CHECK(JOIST_PTE_PERMS(pte(T, 0x401)) == JOIST_MAP_W && pte(T, 0x403) == 0);
    JT_REQUIRE(joist_protect(&k, root, TK, 0x402, 1, JOIST_MAP_X) == JOIST_OK);
    JT_CHECK(JOIST_PTE_PERMS(pte(T, 0x402)) == JOIST_MAP_X);
    JT_CHECK(joist_protect(&k, root, TK, 0x800, 4, 0) == JOIST_OK); /* no table: nothing to do */
    JT_CHECK(clean());
}

JT_TEST(map_frame_dying_is_unmapped_everywhere)
{
    setup();
    JT_REQUIRE(joist_task_create(&k, root, 4, 7) == JOIST_OK);
    joist_cspace U = space_of(7);
    JT_REQUIRE(joist_space_donate(&k, root, 7, MEM2) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 0, 4, TK, 0x400, 0) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 2, 4, 7, 0x800, 0) == JOIST_OK); /* shared */
    uint32_t fo = k.cs[root].slot[F].obj - 1u;
    uint32_t q = k.cs[root].quota;
    k.preempt = 1; /* one table per step */
    JT_REQUIRE(joist_cap_delete(&k, root, F) == JOIST_OK);
    JT_CHECK(k.obj[fo].type == JOIST_OBJ_FRAME && k.obj[fo].dying && k.frames_dying == 1);
    JT_CHECK(k.cs[root].quota == q); /* still charged while it is kept */
    JT_CHECK(joist_boot_frame(&k, root, F, 0x102, 1) == JOIST_E_STATE); /* its pages are still its */
    JT_CHECK(clean());
    unsigned steps = 0;
    while (joist_continue(&k, 1) == JOIST_BLOCKED && steps < 100000u) {
        steps++;
        JT_CHECK(clean());
    }
    k.preempt = 0;
    JT_CHECK(steps > 1); /* it took several steps */
    JT_CHECK(k.obj[fo].type == JOIST_OBJ_NONE && k.frames_dying == 0 && k.cs[root].quota == q + 1u);
    JT_CHECK(pte(T, 0x400) == 0 && pte(U, 0x805) == 0 && k.cs[T].pt_used == 0 && k.cs[U].pt_used == 0);
    JT_CHECK(clean());
}

JT_TEST(map_task_destroy_returns_its_lent_pages)
{
    setup();
    JT_REQUIRE(joist_map(&k, root, F, 0, 3, TK, 0x400, 0) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 0, 3, TK, (uint64_t)1 << JOIST_PT_SHIFT(3), 0) == JOIST_OK);
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK);
    JT_CHECK(frame(F)->mapcount == 0 && frame(MEM)->lent == 0 && frame(MEM)->water == 0);
    JT_CHECK(!k.cs[T].live && k.cs[T].pt_top[0] == 0);
    /* An ordinary frame again: it can be mapped and lent anew. */
    JT_REQUIRE(joist_task_create(&k, root, 4, 7) == JOIST_OK);
    JT_CHECK(joist_space_donate(&k, root, 7, MEM) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(map_lent_frame_dying_waits_for_its_space)
{
    setup();
    uint32_t mo = k.cs[root].slot[MEM].obj - 1u;
    JT_REQUIRE(joist_map(&k, root, F, 0, 1, TK, 0x400, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, root, MEM) == JOIST_OK);
    JT_CHECK(k.obj[mo].type == JOIST_OBJ_FRAME && k.obj[mo].dying && k.frames_dying == 0);
    JT_REQUIRE(joist_map(&k, root, F, 1, 1, TK, 0x401, 0) == JOIST_OK); /* T's tables stay */
    JT_CHECK(clean());
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK);
    JT_CHECK(k.obj[mo].type == JOIST_OBJ_NONE); /* freed with its space */
    JT_CHECK(clean());
}

JT_TEST(map_into_own_space_needs_a_task_capability)
{
    /* No JOIST_SLOT_SELF any more (0020): the root maps into itself with
     * the capability joist_boot_self gave it. */
    setup();
    JT_REQUIRE(joist_space_donate(&k, root, SELF, MEM2) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 0, 2, SELF, 0x600, JOIST_MAP_W) == JOIST_OK);
    JT_CHECK(pte(root, 0x601) != 0);
    JT_CHECK(joist_unmap_range(&k, root, JOIST_SLOT_SELF, 0x600, 2) == JOIST_E_RANGE);
    JT_REQUIRE(joist_unmap_range(&k, root, SELF, 0x600, 2) == JOIST_OK);
    JT_CHECK(pte(root, 0x600) == 0 && k.cs[root].pt_used == 0);
    JT_CHECK(clean());
}

JT_TEST(map_invariants_catch_bad_entries)
{
    setup();
    char msg[200];
    JT_REQUIRE(joist_boot_frame(&k, root, F2, 0x200, 4) == JOIST_OK);
    k.obj[k.cs[root].slot[F2].obj - 1u].base = 0x10e; /* now overlapping F */
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    k.obj[k.cs[root].slot[F2].obj - 1u].base = 0x200;
    JT_REQUIRE(joist_map(&k, root, F, 0, 2, TK, 0x400, JOIST_MAP_W) == JOIST_OK);
    JT_CHECK(clean());
    /* Each corruption alone: the checker sees it. */
    uint64_t *lf = page(NULL, JOIST_PT_SHADOW(((const uint64_t *)page(
                                     NULL, JOIST_PT_SHADOW(((const uint64_t *)page(
                                               NULL, JOIST_PT_SHADOW(k.cs[T].pt_top[0])))[0])))[2]));
    uint64_t saved = lf[0];
    lf[0] = saved | JOIST_PTE_X; /* writable and executable */
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    lf[0] = saved & ~(uint64_t)JOIST_PTE_CEIL_W; /* writable beyond its ceiling */
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    lf[0] = 0; /* the count no longer agrees */
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    lf[0] = saved;
    k.cs[T].pt_avail++;
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    k.cs[T].pt_avail--;
    JT_CHECK(clean());
}
