/* SPDX-License-Identifier: GPL-3.0-only */
/* test_mm.c - unit tests for the memory manager's library (decision 0023;
 * Phase 4 step 6, part A) against the kernel model.
 *
 * The root space plays mm: it holds the pool frames and a task capability
 * with CONTROL for the task T it serves, whose page tables are paid for
 * with a frame lent to it (0021). mm's kernel calls are the model's. What a
 * task reads is found through the kernel model's page tables (the frame and
 * page each entry names) in simulated memory, never through mm's records,
 * so the tests check mm against the kernel. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"
#include "mmlib.h"

#include <string.h>

static struct joist_kernel k;
static joist_cspace root, T, C, D;
static struct mm m;
static uint32_t t, c, d; /* mm's indices for T, C and D */

/* Root's slots: T, the task mm serves; C and D, two more (a clone's child,
 * a task to share with); a frame lent to each for its tables; the pool, and
 * its second part when a test splits it. */
enum { TK = 0, MEM = 1, SELF = 2, POOL = 3, TC = 4, TD = 5, MEMC = 6, MEMD = 7, POOL2 = 8,
       TE = 9, MEME = 10, GROWF = 11, GROW0 = 20 /* to 39: table runs lent on demand */ };

/* Memory for the tasks' page tables (lent pages), as test_map.c: 32 pages
 * each, from MEM_BASE. */
#define MEM_BASE 0x4000u
/* And, for the tests of page tables on demand, E's 6 (MEME, at MEM_BASE +
 * 96) and GROWF_PAGES for the runs lent on demand (at MEM_BASE + 102). */
#define GROWF_PAGES (6u * MM_TABLE_GROW + 1u)
#define MEM_PAGES (102u + GROWF_PAGES)
static uint64_t mem[MEM_PAGES][JOIST_PAGE_SIZE / 8u];

/* The pool's frame: POOL_PAGES pages at POOL_BASE, its bytes simulated. */
#define POOL_BASE 0x9000u
#define POOL_PAGES 48u
static uint8_t pool[POOL_PAGES][JOIST_PAGE_SIZE];

static void *page(void *ctx, uint64_t pfn)
{
    (void)ctx;
    return pfn >= MEM_BASE && pfn < MEM_BASE + MEM_PAGES ? mem[pfn - MEM_BASE] : NULL;
}

/* ---- mm's kernel calls, on the model ---- */

/* mm's map calls, counted; the one numbered map_fail (from 1) fails, as
 * the kernel would, changing nothing. */
static unsigned maps, map_fail;
_Static_assert(MM_KE_OCCUPIED == JOIST_E_OCCUPIED && MM_KE_STATE == JOIST_E_STATE,
               "mmlib's copies of the kernel's errors");

static int k_map(void *ctx, uint32_t fs, uint64_t off, uint64_t n, uint32_t ts, uint64_t va,
                 unsigned perms)
{
    (void)ctx;
    if (++maps == map_fail) {
        return JOIST_E_NOMEM;
    }
    return (int)joist_map(&k, root, fs, off, n, ts, va, perms);
}

static int k_map_replace(void *ctx, uint32_t fs, uint64_t off, uint32_t ts, uint64_t va,
                         unsigned perms, uint32_t ofs, uint64_t ooff)
{
    (void)ctx;
    return (int)joist_map_replace(&k, root, fs, off, 1, ts, va, perms, ofs, ooff);
}

static int k_unmap(void *ctx, uint32_t ts, uint64_t va, uint64_t n)
{
    (void)ctx;
    return joist_unmap_range(&k, root, ts, va, n) != JOIST_OK;
}

static int k_protect(void *ctx, uint32_t ts, uint64_t va, uint64_t n, unsigned perms)
{
    (void)ctx;
    return joist_protect(&k, root, ts, va, n, perms) != JOIST_OK;
}

/* The pool's frame, or its two halves when a test splits it (POOL keeps
 * the first pool_split pages, POOL2 has the rest). */
static uint64_t pool_split = POOL_PAGES;

static uint8_t *k_page(void *ctx, uint32_t fs, uint64_t off)
{
    (void)ctx;
    if (fs == POOL2) {
        off += pool_split;
    } else if (fs != POOL || off >= pool_split) {
        return NULL;
    }
    return off < POOL_PAGES ? pool[off] : NULL;
}

/* Regions (0030): mm's are in root's slots from KREG. */
_Static_assert(MM_KE_NOMEM == JOIST_E_NOMEM && MM_KE_RANGE == JOIST_E_RANGE && MM_R_MAP == JOIST_R_MAP &&
                   MM_QUERY_MAX == JOIST_QUERY_MAX && MM_QUERY_PRESENT == JOIST_QUERY_PRESENT &&
                   MM_QUERY_FRAME == JOIST_QUERY_FRAME && MM_REGION_EXTENTS == JOIST_REGION_EXTENTS &&
                   MM_REGION_TABLES == JOIST_REGION_TABLES,
               "mmlib's copies of the kernel's region constants");
#define KREG 40u

static int k_region_create(void *ctx, uint32_t ts, uint64_t lo, uint32_t tables, unsigned ceil,
                           uint32_t dst)
{
    (void)ctx;
    return (int)joist_region_create(&k, root, ts, lo, tables, ceil, dst);
}

static int k_region_add(void *ctx, uint32_t rs, uint32_t fs, uint64_t off, uint64_t n, uint32_t *ext)
{
    (void)ctx;
    return (int)joist_region_add(&k, root, rs, fs, off, n, ext);
}

static int k_region_map(void *ctx, uint32_t rs, uint32_t ext, uint64_t pg, uint64_t n, uint64_t va,
                        unsigned perms)
{
    (void)ctx;
    return (int)joist_region_map(&k, root, rs, ext, pg, n, va, perms);
}

static int k_query(void *ctx, uint32_t ts, uint64_t va, uint64_t n, uint32_t fs, uint64_t *out)
{
    (void)ctx;
    return (int)joist_map_query(&k, root, ts, va, n, fs, out);
}

static int k_give(void *ctx, uint32_t ts, uint32_t src, uint32_t dst, unsigned mask)
{
    (void)ctx;
    return (int)joist_cap_give(&k, root, ts, src, dst, (joist_rights)mask);
}

static int k_kill(void *ctx, uint32_t slot)
{
    (void)ctx;
    return (int)joist_obj_kill(&k, root, slot);
}

static int k_del(void *ctx, uint32_t slot)
{
    (void)ctx;
    return (int)joist_cap_delete(&k, root, slot);
}

/* The kernel's pending work run, at most finish_steps steps of one unit. */
static unsigned finish_steps = 1000u;

static int k_finish(void *ctx)
{
    (void)ctx;
    for (unsigned i = 0; i < finish_steps; i++) {
        if (joist_continue(&k, 1) != JOIST_BLOCKED) {
            return 1;
        }
    }
    return 0;
}

/* Page tables on demand (0021): runs of MM_TABLE_GROW pages split off the
 * end of GROWF, one more slot from GROW0 each, while grow_left lasts; runs
 * given back (undonate) are lent again first, once the kernel has let them
 * go. Off unless a test turns it on (grow_on), so mm's other tests see a
 * task's first lent frame alone, as before. */
static int grow_on;
static unsigned donates, undonates;
static uint64_t grow_left;
static uint32_t grow_next;
static uint32_t grow_back[8];
static unsigned nback;

static int k_donate(void *ctx, uint32_t ts, uint32_t *run)
{
    (void)ctx;
    donates++;
    if (!grow_on) {
        return JOIST_E_NOMEM;
    }
    for (unsigned i = 0; i < nback; i++) {
        if (joist_space_donate(&k, root, ts, grow_back[i]) == JOIST_OK) {
            *run = grow_back[i];
            grow_back[i] = grow_back[--nback];
            return 0;
        }
    }
    if (grow_left <= MM_TABLE_GROW) {
        return JOIST_E_NOMEM;
    }
    uint32_t r = grow_next;
    if (joist_frame_split(&k, root, GROWF, grow_left - MM_TABLE_GROW, r) != JOIST_OK) {
        return JOIST_E_NOMEM;
    }
    grow_next++;
    grow_left -= MM_TABLE_GROW;
    int e = (int)joist_space_donate(&k, root, ts, r);
    if (e != JOIST_OK) {
        grow_back[nback++] = r;
        return e;
    }
    *run = r;
    return 0;
}

static void k_undonate(void *ctx, uint32_t run)
{
    (void)ctx;
    undonates++;
    if (nback < sizeof grow_back / sizeof grow_back[0]) {
        grow_back[nback++] = run;
    }
}

static const struct mm_kops kops = {
    .ctx = NULL, .map = k_map, .map_replace = k_map_replace, .unmap = k_unmap,
    .protect = k_protect, .page = k_page, .region_create = k_region_create,
    .region_add = k_region_add, .region_map = k_region_map, .query = k_query, .give = k_give,
    .kill = k_kill, .del = k_del, .finish = k_finish, .donate = k_donate, .undonate = k_undonate};

/* ---- the world ---- */

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    unsigned b = mm_check(&m, msg, sizeof msg);
    if (b != 0) {
        fprintf(stderr, "  mm: %u fault(s), first: %s\n", b, msg);
    }
    return n == 0 && b == 0;
}

static void setup(void)
{
    joist_init(&k);
    maps = 0;
    map_fail = 0;
    pool_split = POOL_PAGES;
    grow_on = 0;
    donates = undonates = 0;
    grow_left = 0;
    grow_next = GROW0;
    nback = 0;
    finish_steps = 1000u;
    memset(mem, 0, sizeof mem);
    memset(pool, 0xa5, sizeof pool); /* a page mm hands out must be zeroed */
    k.hooks.page = page;
    if (joist_boot(&k, 128, &root) != JOIST_OK ||
        joist_boot_frame(&k, root, MEM, MEM_BASE, 32) != JOIST_OK ||
        joist_boot_frame(&k, root, POOL, POOL_BASE, POOL_PAGES) != JOIST_OK ||
        joist_boot_frame(&k, root, MEMC, MEM_BASE + 32u, 32) != JOIST_OK ||
        joist_boot_frame(&k, root, MEMD, MEM_BASE + 64u, 32) != JOIST_OK ||
        joist_boot_self(&k, root, SELF) != JOIST_OK || joist_task_create(&k, root, 20, TK) != JOIST_OK ||
        joist_space_donate(&k, root, TK, MEM) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    T = k.obj[k.cs[root].slot[TK].obj - 1u].target - 1u;
    mm_init(&m, &kops);
    m.kreg_slot = KREG;
    /* T's tree may have the whole pool (the trees' limits have tests of
     * their own: 0028). */
    if (mm_add_frame(&m, POOL, POOL_PAGES) != MM_OK || mm_task_add(&m, TK, &t) != MM_OK ||
        mm_set_limit(&m, t, 0, POOL_PAGES, MM_MAX_TASKS) != MM_OK) {
        jt_fail(__FILE__, __LINE__, "mm could not be set up as written", 0);
    }
}

/* C and D, served by mm too, each with its tables' pages: C in T's tree
 * (a clone's child), D a tree of its own. */
static void more_tasks(void)
{
    if (joist_task_create(&k, root, 20, TC) != JOIST_OK || joist_space_donate(&k, root, TC, MEMC) != JOIST_OK ||
        joist_task_create(&k, root, 20, TD) != JOIST_OK || joist_space_donate(&k, root, TD, MEMD) != JOIST_OK ||
        mm_task_add_child(&m, TC, t, &c) != MM_OK || mm_task_add(&m, TD, &d) != MM_OK ||
        mm_set_limit(&m, d, 0, POOL_PAGES, MM_MAX_TASKS) != MM_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    C = k.obj[k.cs[root].slot[TC].obj - 1u].target - 1u;
    D = k.obj[k.cs[root].slot[TD].obj - 1u].target - 1u;
}

/* The kernel's entry for page va of space s, 0 if none. */
static uint64_t pte_in(joist_cspace s, uint64_t va)
{
    uint64_t e = k.cs[s].pt_top[va >> JOIST_PT_SHIFT(3)];
    for (unsigned l = 2; e != 0 && l > 0; l--) {
        e = ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[(va >> JOIST_PT_SHIFT(l)) &
                                                                (JOIST_PT_ENTRIES - 1u)];
    }
    return e == 0 ? 0 : ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[va & (JOIST_PT_ENTRIES - 1u)];
}

static uint64_t pte(uint64_t va)
{
    return pte_in(T, va);
}

/* The bytes space s sees at page va, through the kernel's entry: NULL if
 * unmapped. */
static uint8_t *seen_in(joist_cspace s, uint64_t va)
{
    uint64_t e = pte_in(s, va);
    if (e == 0) {
        return NULL;
    }
    const struct joist_object *f = &k.obj[JOIST_PTE_FRAME(e)];
    uint64_t pfn = f->base + JOIST_PTE_OFF(e);
    return pfn >= POOL_BASE && pfn < POOL_BASE + POOL_PAGES ? pool[pfn - POOL_BASE] : NULL;
}

static uint8_t *seen(uint64_t va)
{
    return seen_in(T, va);
}

/* ---- tests ---- */

JT_TEST(mm_read_fault_maps_zeroed_pages_around)
{
    setup();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 40, MM_W) == MM_OK);
    JT_CHECK(pte(0x400) == 0); /* nothing until touched */
    JT_CHECK(mm_fault(&m, t, 0x402, MM_ACCESS_READ) == 1);
    /* The faulting page and the next ones, up to MM_AROUND, zeroed. */
    for (uint64_t va = 0x402; va < 0x402 + MM_AROUND; va++) {
        uint8_t *b = seen(va);
        JT_CHECK(b != NULL && b[0] == 0 && b[JOIST_PAGE_SIZE - 1u] == 0);
        JT_CHECK((pte(va) & JOIST_PTE_W) != 0); /* the region's permissions */
    }
    JT_CHECK(pte(0x401) == 0 && pte(0x402 + MM_AROUND) == 0);
    JT_CHECK(m.nfree == POOL_PAGES - MM_AROUND);
    /* A read fault next to mapped pages stops at them. */
    JT_CHECK(mm_fault(&m, t, 0x400, MM_ACCESS_READ) == 1);
    JT_CHECK(pte(0x400) != 0 && pte(0x401) != 0 && m.nfree == POOL_PAGES - MM_AROUND - 2u);
    JT_CHECK(clean());
}

JT_TEST(mm_write_fault_maps_one_page_and_keeps_what_is_written)
{
    setup();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 8, MM_W) == MM_OK);
    JT_CHECK(mm_fault(&m, t, 0x405, MM_ACCESS_WRITE) == 1);
    JT_CHECK(seen(0x405) != NULL && pte(0x404) == 0 && pte(0x406) == 0);
    seen(0x405)[7] = 0x42;
    /* Another thread's fault on the same page finds it there. */
    JT_CHECK(mm_fault(&m, t, 0x405, MM_ACCESS_WRITE) == 1);
    JT_CHECK(seen(0x405)[7] == 0x42 && m.nfree == POOL_PAGES - 1u);
    JT_CHECK(clean());
}

JT_TEST(mm_fault_refused_outside_regions_and_their_permissions)
{
    setup();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 4, 0) == MM_OK);    /* read-only */
    JT_REQUIRE(mm_map_anon(&m, t, 0x500, 4, MM_X) == MM_OK); /* read and execute */
    JT_CHECK(mm_fault(&m, t, 0x3ff, MM_ACCESS_READ) == 0);
    JT_CHECK(mm_fault(&m, t, 0x404, MM_ACCESS_READ) == 0);
    JT_CHECK(mm_fault(&m, t, 0x400, MM_ACCESS_WRITE) == 0);
    JT_CHECK(mm_fault(&m, t, 0x400, MM_ACCESS_EXEC) == 0);
    JT_CHECK(mm_fault(&m, t, 0x500, MM_ACCESS_EXEC) == 1);
    JT_CHECK((pte(0x500) & JOIST_PTE_X) != 0 && (pte(0x500) & JOIST_PTE_W) == 0);
    /* A present page written where the region forbids it. */
    JT_CHECK(mm_fault(&m, t, 0x500, MM_ACCESS_WRITE | MM_ACCESS_PRESENT) == 0);
    JT_CHECK(m.nfree == POOL_PAGES - 4u && clean());
}

JT_TEST(mm_map_anon_refusals)
{
    setup();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 8, MM_W) == MM_OK);
    JT_CHECK(mm_map_anon(&m, t, 0x407, 2, MM_W) == MM_E_STATE); /* overlaps */
    JT_CHECK(mm_map_anon(&m, t, 0x408, 0, MM_W) == MM_E_RANGE);
    JT_CHECK(mm_map_anon(&m, t, UINT64_MAX, 2, MM_W) == MM_E_RANGE);
    JT_CHECK(mm_map_anon(&m, t, 0x408, 1, MM_W | MM_X) == MM_E_RANGE); /* W^X */
    JT_CHECK(mm_map_anon(&m, t, 0x408, 1, 0x8u) == MM_E_RANGE); /* unknown */
    JT_CHECK(mm_map_anon(&m, t + 1u, 0x408, 1, MM_W) == MM_E_STATE); /* not served */
    for (unsigned i = 1; i < MM_REGIONS; i++) {
        JT_REQUIRE(mm_map_anon(&m, t, 0x1000u + 16u * i, 4, 0) == MM_OK);
    }
    JT_CHECK(mm_map_anon(&m, t, 0x2000, 4, 0) == MM_E_NOMEM); /* the table is full */
    JT_CHECK(clean());
}

JT_TEST(mm_unmap_splits_regions_and_frees_pages)
{
    setup();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 16, MM_W) == MM_OK);
    JT_REQUIRE(mm_fault(&m, t, 0x400, MM_ACCESS_READ) == 1); /* all 16 */
    JT_REQUIRE(m.nfree == POOL_PAGES - 16u);
    JT_CHECK(mm_unmap(&m, t, 0x404, 4) == MM_OK); /* a hole in the middle */
    for (uint64_t va = 0x400; va < 0x410; va++) {
        JT_CHECK((pte(va) != 0) == (va < 0x404 || va >= 0x408));
    }
    JT_CHECK(m.nfree == POOL_PAGES - 12u);
    JT_CHECK(mm_fault(&m, t, 0x405, MM_ACCESS_READ) == 0); /* the hole is no region now */
    /* Unmapping where nothing is: fine, nothing changes. */
    JT_CHECK(mm_unmap(&m, t, 0x2000, 8) == MM_OK);
    /* Across a table's span (two kernel calls), and the regions' edges. */
    JT_REQUIRE(mm_map_anon(&m, t, MM_SPAN - 4u, 8, MM_W) == MM_OK);
    JT_REQUIRE(mm_fault(&m, t, MM_SPAN - 4u, MM_ACCESS_READ) == 1);
    JT_CHECK(pte(MM_SPAN - 1u) != 0 && pte(MM_SPAN) == 0); /* stops at the span */
    JT_REQUIRE(mm_fault(&m, t, MM_SPAN, MM_ACCESS_READ) == 1);
    JT_CHECK(pte(MM_SPAN - 1u) != 0 && pte(MM_SPAN + 3u) != 0);
    JT_CHECK(mm_unmap(&m, t, MM_SPAN - 4u, 8) == MM_OK);
    JT_CHECK(pte(MM_SPAN - 1u) == 0 && pte(MM_SPAN + 3u) == 0);
    JT_CHECK(m.nfree == POOL_PAGES - 12u && clean());
}

JT_TEST(mm_unmap_refused_when_a_split_has_no_room)
{
    setup();
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        JT_REQUIRE(mm_map_anon(&m, t, 0x1000u + 16u * i, 8, 0) == MM_OK);
    }
    JT_CHECK(mm_unmap(&m, t, 0x1002, 2) == MM_E_NOMEM); /* would split, no free entry */
    JT_CHECK(mm_unmap(&m, t, 0x1000, 8) == MM_OK);      /* a whole region: fine */
    JT_CHECK(mm_unmap(&m, t, 0x1012, 2) == MM_OK);      /* now there is room */
    JT_CHECK(clean());
}

JT_TEST(mm_protect_changes_regions_and_mapped_pages)
{
    setup();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 16, MM_W) == MM_OK);
    JT_REQUIRE(mm_fault(&m, t, 0x400, MM_ACCESS_READ) == 1);
    JT_CHECK(mm_protect(&m, t, 0x404, 4, 0) == MM_OK);
    for (uint64_t va = 0x400; va < 0x410; va++) {
        JT_CHECK(((pte(va) & JOIST_PTE_W) != 0) == (va < 0x404 || va >= 0x408));
    }
    JT_CHECK(mm_fault(&m, t, 0x405, MM_ACCESS_WRITE | MM_ACCESS_PRESENT) == 0);
    JT_CHECK(mm_protect(&m, t, 0x404, 4, MM_W) == MM_OK); /* back */
    JT_CHECK((pte(0x405) & JOIST_PTE_W) != 0);
    /* The whole range must be in regions; W^X refused. */
    JT_CHECK(mm_protect(&m, t, 0x40c, 8, 0) == MM_E_STATE);
    JT_CHECK(mm_protect(&m, t, 0x400, 2, MM_W | MM_X) == MM_E_RANGE);
    JT_CHECK(clean());
}

JT_TEST(mm_pool_runs_out_and_the_fault_ends_the_thread)
{
    setup();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 2u * POOL_PAGES, MM_W) == MM_OK);
    for (uint64_t va = 0x400; va < 0x400 + POOL_PAGES; va++) {
        JT_REQUIRE(mm_fault(&m, t, va, MM_ACCESS_WRITE) == 1);
    }
    JT_CHECK(m.nfree == 0);
    JT_CHECK(mm_fault(&m, t, 0x400 + POOL_PAGES, MM_ACCESS_WRITE) == 0);
    JT_CHECK(pte(0x400 + POOL_PAGES) == 0 && clean());
}

JT_TEST(mm_task_gone_frees_its_pages)
{
    setup();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 8, MM_W) == MM_OK);
    JT_REQUIRE(mm_fault(&m, t, 0x400, MM_ACCESS_READ) == 1);
    JT_REQUIRE(m.nfree == POOL_PAGES - 8u);
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK);
    mm_task_gone(&m, t);
    JT_CHECK(m.nfree == POOL_PAGES && !m.task[t].live);
    JT_CHECK(mm_map_anon(&m, t, 0x400, 8, MM_W) == MM_E_STATE);
    JT_CHECK(clean());
}

/* ---- part B: clone, copy on write, sharing ---- */

/* T with 4 written pages at 0x400 (bytes 0x11, 0x12, ...). */
static void t_with_pages(void)
{
    more_tasks();
    if (mm_map_anon(&m, t, 0x400, 8, MM_W) != MM_OK) {
        jt_fail(__FILE__, __LINE__, "map_anon", 0);
        return;
    }
    for (uint64_t i = 0; i < 4; i++) {
        if (mm_fault(&m, t, 0x400 + i, MM_ACCESS_WRITE) != 1) {
            jt_fail(__FILE__, __LINE__, "fault", 0);
            return;
        }
        seen(0x400 + i)[0] = (uint8_t)(0x11u + i);
    }
}

JT_TEST(mm_clone_maps_the_pages_read_only_into_the_child)
{
    setup();
    t_with_pages();
    uint32_t used = POOL_PAGES - m.nfree;
    JT_REQUIRE(mm_clone(&m, t, c) == MM_OK);
    for (uint64_t i = 0; i < 4; i++) {
        uint64_t pe = pte(0x400 + i);
        uint64_t ce = pte_in(C, 0x400 + i);
        /* The same page, read-only in both. */
        JT_CHECK(pe != 0 && ce != 0 && JOIST_PTE_FRAME(pe) == JOIST_PTE_FRAME(ce) &&
                 JOIST_PTE_OFF(pe) == JOIST_PTE_OFF(ce));
        JT_CHECK((pe & JOIST_PTE_W) == 0 && (ce & JOIST_PTE_W) == 0);
        JT_CHECK(seen_in(C, 0x400 + i)[0] == 0x11u + i);
    }
    JT_CHECK(POOL_PAGES - m.nfree == used); /* nothing copied yet */
    JT_CHECK(pte_in(C, 0x404) == 0 && m.task[c].reg[0].n != 0); /* the regions too */
    JT_CHECK(clean());
}

JT_TEST(mm_cow_write_copies_then_upgrades_the_last)
{
    setup();
    t_with_pages();
    JT_REQUIRE(mm_clone(&m, t, c) == MM_OK);
    uint32_t used = POOL_PAGES - m.nfree;
    /* The child writes: it gets a copy, the parent keeps the original. */
    JT_CHECK(mm_fault(&m, c, 0x401, MM_ACCESS_WRITE | MM_ACCESS_PRESENT) == 1);
    JT_CHECK((pte_in(C, 0x401) & JOIST_PTE_W) != 0 && POOL_PAGES - m.nfree == used + 1u);
    JT_CHECK(seen_in(C, 0x401)[0] == 0x12 && seen(0x401)[0] == 0x12);
    seen_in(C, 0x401)[0] = 0x99;
    JT_CHECK(seen(0x401)[0] == 0x12); /* not the parent's */
    /* The parent writes the same page: now its last reference, so no copy. */
    JT_CHECK(mm_fault(&m, t, 0x401, MM_ACCESS_WRITE | MM_ACCESS_PRESENT) == 1);
    JT_CHECK((pte(0x401) & JOIST_PTE_W) != 0 && POOL_PAGES - m.nfree == used + 1u);
    /* A page still shared stays read-only for the other. */
    JT_CHECK(mm_fault(&m, t, 0x402, MM_ACCESS_WRITE | MM_ACCESS_PRESENT) == 1);
    JT_CHECK((pte_in(C, 0x402) & JOIST_PTE_W) == 0 && seen_in(C, 0x402)[0] == 0x13);
    JT_CHECK(clean());
}

JT_TEST(mm_clone_refusals)
{
    setup();
    t_with_pages();
    JT_CHECK(mm_clone(&m, t, t) == MM_E_STATE);
    JT_REQUIRE(mm_map_anon(&m, c, 0x800, 1, 0) == MM_OK);
    JT_CHECK(mm_clone(&m, t, c) == MM_E_STATE); /* not a new task */
    JT_CHECK(mm_clone(&m, t, d + 1u) == MM_E_STATE);
    JT_CHECK(clean());
}

JT_TEST(mm_share_maps_the_same_pages_and_keeps_them_shared_in_a_clone)
{
    setup();
    t_with_pages();
    JT_REQUIRE(mm_share(&m, t, 0x401, 3, d, 0x900) == MM_OK); /* 0x401-0x403, all mapped */
    for (uint64_t i = 0; i < 3; i++) {
        uint64_t te = pte(0x401 + i);
        uint64_t de = pte_in(D, 0x900 + i);
        JT_CHECK(te != 0 && de != 0 && JOIST_PTE_FRAME(te) == JOIST_PTE_FRAME(de) &&
                 JOIST_PTE_OFF(te) == JOIST_PTE_OFF(de) && (de & JOIST_PTE_W) != 0);
    }
    seen_in(D, 0x900)[1] = 0x77;
    JT_CHECK(seen(0x401)[1] == 0x77); /* one page */
    /* A clone of T maps the shared pages writable, still the same page;
     * the private one read-only. */
    JT_REQUIRE(mm_clone(&m, t, c) == MM_OK);
    JT_CHECK((pte_in(C, 0x401) & JOIST_PTE_W) != 0 && seen_in(C, 0x401)[1] == 0x77);
    JT_CHECK((pte_in(C, 0x400) & JOIST_PTE_W) == 0 && (pte(0x400) & JOIST_PTE_W) == 0);
    seen_in(C, 0x402)[2] = 0x55;
    JT_CHECK(seen(0x402)[2] == 0x55 && seen_in(D, 0x901)[2] == 0x55);
    /* A write fault on a shared page that isn't writable: made so, no copy. */
    JT_REQUIRE(joist_protect(&k, root, TK, 0x401, 1, 0) == JOIST_OK);
    uint32_t used = POOL_PAGES - m.nfree;
    JT_CHECK(mm_fault(&m, t, 0x401, MM_ACCESS_WRITE | MM_ACCESS_PRESENT) == 1);
    JT_CHECK((pte(0x401) & JOIST_PTE_W) != 0 && POOL_PAGES - m.nfree == used);
    JT_CHECK(clean());
}

JT_TEST(mm_share_copies_a_cow_page_first_and_makes_missing_ones)
{
    setup();
    t_with_pages();
    JT_REQUIRE(mm_clone(&m, t, c) == MM_OK); /* T's pages now COW-shared with C */
    uint32_t used = POOL_PAGES - m.nfree;
    JT_REQUIRE(mm_share(&m, t, 0x403, 3, d, 0x900) == MM_OK); /* 0x403 COW; 0x404-5 absent */
    JT_CHECK(POOL_PAGES - m.nfree == used + 3u); /* a copy and two new pages */
    JT_CHECK(seen(0x403)[0] == 0x14 && seen_in(C, 0x403)[0] == 0x14);
    seen(0x403)[0] = 0x66;
    JT_CHECK(seen_in(D, 0x900)[0] == 0x66 && seen_in(C, 0x403)[0] == 0x14); /* C keeps its own */
    JT_CHECK(pte(0x404) != 0 && pte_in(D, 0x901) != 0 && seen_in(D, 0x902)[0] == 0);
    JT_CHECK(clean());
}

JT_TEST(mm_share_refusals_and_unmapping_one_side)
{
    setup();
    t_with_pages();
    JT_CHECK(mm_share(&m, t, 0x406, 4, d, 0x900) == MM_E_STATE); /* past T's region */
    JT_CHECK(mm_share(&m, t, 0x3ff, 2, d, 0x900) == MM_E_STATE); /* starts outside */
    JT_CHECK(mm_share(&m, t, 0x400, 2, t, 0x900) == MM_E_STATE); /* itself */
    JT_REQUIRE(mm_map_anon(&m, d, 0x901, 1, 0) == MM_OK);
    JT_CHECK(mm_share(&m, t, 0x400, 2, d, 0x900) == MM_E_STATE); /* D has a region there */
    JT_CHECK(mm_share(&m, t, 0x400, 0, d, 0x700) == MM_E_RANGE);
    JT_REQUIRE(mm_share(&m, t, 0x400, 2, d, 0x700) == MM_OK);
    /* T unmaps its side: D keeps the pages and what is in them. */
    uint32_t used = POOL_PAGES - m.nfree;
    JT_REQUIRE(mm_unmap(&m, t, 0x400, 2) == MM_OK);
    JT_CHECK(pte(0x400) == 0 && seen_in(D, 0x700)[0] == 0x11 && POOL_PAGES - m.nfree == used);
    JT_REQUIRE(mm_unmap(&m, d, 0x700, 2) == MM_OK);
    JT_CHECK(POOL_PAGES - m.nfree == used - 2u);
    JT_CHECK(clean());
}

/* ---- runs: contiguous pages mapped with one call ---- */

/* mm's records of task ti and space s's mappings agree over [lo, hi): a
 * page is mapped exactly where mm has a record of it, and is the page the
 * record names. */
static int agrees(uint32_t ti, joist_cspace s, uint64_t lo, uint64_t hi)
{
    const struct mm_task *tk = &m.task[ti];
    for (uint64_t va = lo; va < hi; va++) {
        const uint8_t *want = NULL;
        for (uint32_t i = 0; i < tk->nmap; i++) {
            if (tk->map[i].va == va) {
                const struct mm_page *g = &m.pg[tk->map[i].page];
                want = k_page(NULL, m.frame[g->frame].slot, g->off);
            }
        }
        if (seen_in(s, va) != want) {
            fprintf(stderr, "  page %llx: mm's records and the kernel's differ\n",
                    (unsigned long long)va);
            return 0;
        }
    }
    return 1;
}

/* mm's pool in two frames, the first of `first` pages. Its free list is
 * put in page order across both (as pages given back in order leave it),
 * so a read fault's pages go on from one frame into the other. */
static void two_frames(uint64_t first)
{
    pool_split = first;
    mm_init(&m, &kops);
    if (joist_frame_split(&k, root, POOL, first, POOL2) != JOIST_OK ||
        mm_add_frame(&m, POOL, first) != MM_OK ||
        mm_add_frame(&m, POOL2, POOL_PAGES - first) != MM_OK || mm_task_add(&m, TK, &t) != MM_OK ||
        mm_set_limit(&m, t, 0, POOL_PAGES, MM_MAX_TASKS) != MM_OK) {
        jt_fail(__FILE__, __LINE__, "mm could not be set up as written", 0);
        return;
    }
    for (uint32_t p = 0; p < POOL_PAGES; p++) {
        m.pg[p].next = p + 1u < POOL_PAGES ? p + 2u : 0;
    }
    m.free = 1;
}

JT_TEST(mm_read_fault_maps_its_pages_in_one_call)
{
    setup();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 40, MM_W) == MM_OK);
    JT_CHECK(mm_fault(&m, t, 0x402, MM_ACCESS_READ) == 1);
    JT_CHECK(maps == 1); /* 16 pages, one run */
    /* Unmapped, asked for and read again: the pages come back in order,
     * still one run. */
    JT_REQUIRE(mm_unmap(&m, t, 0x400, 40) == MM_OK);
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 40, MM_W) == MM_OK);
    maps = 0;
    JT_CHECK(mm_fault(&m, t, 0x400, MM_ACCESS_READ) == 1 && maps == 1);
    for (uint64_t i = 0; i < MM_AROUND; i++) {
        JT_CHECK(JOIST_PTE_OFF(pte(0x400 + i)) == i);
    }
    JT_CHECK(agrees(t, T, 0x3f0, 0x430) && clean());
}

JT_TEST(mm_runs_stop_at_a_frame)
{
    setup();
    two_frames(3);
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 40, MM_W) == MM_OK);
    JT_CHECK(mm_fault(&m, t, 0x400, MM_ACCESS_READ) == 1);
    JT_CHECK(maps == 2); /* 3 pages in one frame, 13 in the other */
    JT_CHECK(JOIST_PTE_FRAME(pte(0x402)) != JOIST_PTE_FRAME(pte(0x403)));
    JT_CHECK(JOIST_PTE_OFF(pte(0x403)) == 0 && pte(0x40f) != 0 && pte(0x410) == 0);
    JT_CHECK(agrees(t, T, 0x3f0, 0x430) && clean());
}

JT_TEST(mm_clone_runs_stop_at_gaps_permissions_and_spans)
{
    setup();
    more_tasks();
    /* Pages 0 and 1 at 0x400, 2 at 0x403: consecutive in the pool, not in
     * the task. */
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 8, MM_W) == MM_OK);
    JT_REQUIRE(mm_fault(&m, t, 0x400, MM_ACCESS_WRITE) == 1);
    JT_REQUIRE(mm_fault(&m, t, 0x401, MM_ACCESS_WRITE) == 1);
    JT_REQUIRE(mm_fault(&m, t, 0x403, MM_ACCESS_WRITE) == 1);
    /* Pages 3 to 6 at 0x408: consecutive, but the child maps the first two
     * read-only and the others executable. */
    JT_REQUIRE(mm_map_anon(&m, t, 0x408, 2, MM_W) == MM_OK);
    JT_REQUIRE(mm_map_anon(&m, t, 0x40a, 2, MM_X) == MM_OK);
    JT_REQUIRE(mm_fault(&m, t, 0x408, MM_ACCESS_WRITE) == 1);
    JT_REQUIRE(mm_fault(&m, t, 0x409, MM_ACCESS_WRITE) == 1);
    JT_REQUIRE(mm_fault(&m, t, 0x40a, MM_ACCESS_READ) == 1); /* both */
    /* Pages 7 to 10 across a table's span. */
    JT_REQUIRE(mm_map_anon(&m, t, MM_SPAN - 2u, 4, MM_W) == MM_OK);
    for (uint64_t va = MM_SPAN - 2u; va < MM_SPAN + 2u; va++) {
        JT_REQUIRE(mm_fault(&m, t, va, MM_ACCESS_WRITE) == 1);
    }
    maps = 0;
    JT_REQUIRE(mm_clone(&m, t, c) == MM_OK);
    JT_CHECK(maps == 6); /* 0-1, 2; 3-4, 5-6; 7-8, 9-10 */
    JT_CHECK((pte_in(C, 0x409) & (JOIST_PTE_W | JOIST_PTE_X)) == 0);
    JT_CHECK((pte_in(C, 0x40a) & JOIST_PTE_X) != 0 && (pte_in(C, 0x40a) & JOIST_PTE_W) == 0);
    JT_CHECK(agrees(c, C, 0x3f0, 0x410) && agrees(c, C, MM_SPAN - 4u, MM_SPAN + 4u));
    JT_CHECK(agrees(t, T, 0x3f0, 0x410) && agrees(t, T, MM_SPAN - 4u, MM_SPAN + 4u) && clean());
}

JT_TEST(mm_share_maps_runs_into_both)
{
    setup();
    t_with_pages(); /* pages 0-3 at 0x400 */
    maps = 0;
    JT_REQUIRE(mm_share(&m, t, 0x400, 8, d, 0x900) == MM_OK);
    JT_CHECK(maps == 2); /* T's 4 new pages, then D's 8 */
    JT_CHECK(agrees(t, T, 0x3f0, 0x410) && agrees(d, D, 0x8f0, 0x910) && clean());
}

JT_TEST(mm_read_fault_run_fails_part_way)
{
    setup();
    two_frames(3);
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 40, MM_W) == MM_OK);
    /* The second run fails: the first is there, with the faulting page. */
    map_fail = 2;
    JT_CHECK(mm_fault(&m, t, 0x400, MM_ACCESS_READ) == 1);
    JT_CHECK(pte(0x402) != 0 && pte(0x403) == 0 && m.task[t].nmap == 3);
    JT_CHECK(m.nfree == POOL_PAGES - 3u && agrees(t, T, 0x3f0, 0x430) && clean());
    /* The first fails: nothing is there, and the thread ends. */
    maps = 0;
    map_fail = 1;
    JT_CHECK(mm_fault(&m, t, 0x410, MM_ACCESS_READ) == 0);
    JT_CHECK(pte(0x410) == 0 && m.task[t].nmap == 3 && m.nfree == POOL_PAGES - 3u);
    JT_CHECK(agrees(t, T, 0x3f0, 0x430) && clean());
    /* And the next fault maps them. */
    JT_CHECK(mm_fault(&m, t, 0x410, MM_ACCESS_READ) == 1 && pte(0x41f) != 0);
    JT_CHECK(agrees(t, T, 0x3f0, 0x430) && clean());
}

JT_TEST(mm_clone_run_fails_part_way)
{
    setup();
    more_tasks();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 8, MM_W) == MM_OK);
    JT_REQUIRE(mm_fault(&m, t, 0x400, MM_ACCESS_WRITE) == 1);
    JT_REQUIRE(mm_fault(&m, t, 0x401, MM_ACCESS_WRITE) == 1);
    JT_REQUIRE(mm_fault(&m, t, 0x403, MM_ACCESS_WRITE) == 1);
    maps = 0;
    map_fail = 2; /* the child's second run */
    JT_CHECK(mm_clone(&m, t, c) == MM_E_KERNEL);
    JT_CHECK(m.task[c].nmap == 2 && pte_in(C, 0x401) != 0 && pte_in(C, 0x403) == 0);
    JT_CHECK(agrees(c, C, 0x3f0, 0x410) && clean());
}

JT_TEST(mm_share_run_fails_part_way)
{
    /* D's run fails: T's side is done, D has nothing. */
    setup();
    t_with_pages();
    maps = 0;
    map_fail = 2;
    JT_CHECK(mm_share(&m, t, 0x402, 4, d, 0x900) == MM_E_KERNEL);
    JT_CHECK(pte(0x405) != 0 && pte_in(D, 0x900) == 0 && m.task[d].nmap == 0);
    JT_CHECK(agrees(t, T, 0x3f0, 0x410) && agrees(d, D, 0x8f0, 0x910) && clean());
    /* T's run fails: its new pages are not there, nor in mm's records. */
    setup();
    t_with_pages();
    uint32_t free_before = m.nfree;
    maps = 0;
    map_fail = 1;
    JT_CHECK(mm_share(&m, t, 0x402, 4, d, 0x900) == MM_E_KERNEL);
    JT_CHECK(pte(0x404) == 0 && m.task[t].nmap == 4 && m.nfree == free_before);
    JT_CHECK(agrees(t, T, 0x3f0, 0x410) && agrees(d, D, 0x8f0, 0x910) && clean());
}

/* ---- MM_NONE: no access, and guard pages (0024) ---- */

JT_TEST(mm_none_refuses_every_access_and_cannot_be_mapped_over)
{
    setup();
    more_tasks();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 4, MM_NONE) == MM_OK);
    maps = 0;
    for (uint64_t va = 0x400; va < 0x404; va++) {
        JT_CHECK(mm_fault(&m, t, va, MM_ACCESS_READ) == 0);
        JT_CHECK(mm_fault(&m, t, va, MM_ACCESS_WRITE) == 0);
        JT_CHECK(mm_fault(&m, t, va, MM_ACCESS_EXEC) == 0);
        JT_CHECK(pte(va) == 0);
    }
    JT_CHECK(m.nfree == POOL_PAGES && maps == 0); /* nothing taken, nothing asked of the kernel */
    /* A guard stays: nothing is mapped over it. */
    JT_CHECK(mm_map_anon(&m, t, 0x402, 1, MM_W) == MM_E_STATE);
    /* Alone only. */
    JT_CHECK(mm_map_anon(&m, t, 0x500, 1, MM_NONE | MM_W) == MM_E_RANGE);
    JT_CHECK(mm_map_anon(&m, t, 0x500, 1, MM_NONE | MM_X) == MM_E_RANGE);
    JT_CHECK(mm_protect(&m, t, 0x400, 1, MM_NONE | MM_W) == MM_E_RANGE);
    JT_CHECK(mm_share(&m, t, 0x400, 2, d, 0x900) == MM_E_STATE); /* no pages to share */
    JT_CHECK(clean());
}

JT_TEST(mm_protect_none_and_back_keeps_the_contents)
{
    setup();
    t_with_pages();
    uint32_t free_before = m.nfree;
    JT_REQUIRE(mm_protect(&m, t, 0x401, 2, MM_NONE) == MM_OK);
    JT_CHECK(pte(0x400) != 0 && pte(0x401) == 0 && pte(0x402) == 0 && pte(0x403) != 0);
    JT_CHECK(m.nfree == free_before && m.task[t].nmap == 4); /* mm keeps them */
    maps = 0;
    JT_CHECK(mm_fault(&m, t, 0x401, MM_ACCESS_READ) == 0);
    JT_CHECK(mm_fault(&m, t, 0x402, MM_ACCESS_WRITE) == 0);
    JT_CHECK(maps == 0 && clean()); /* refused by mm, not by the kernel */
    /* Back: mapped again at the first fault, holding what they held. */
    JT_REQUIRE(mm_protect(&m, t, 0x401, 2, MM_W) == MM_OK);
    JT_CHECK(pte(0x401) == 0);
    /* The kernel refusing (not "already there"): the thread is ended, not
     * resumed to fault again for ever; the page is kept. */
    maps = 0;
    map_fail = 1;
    JT_CHECK(mm_fault(&m, t, 0x401, MM_ACCESS_READ) == 0);
    JT_CHECK(pte(0x401) == 0 && m.task[t].nmap == 4);
    map_fail = 0;
    JT_CHECK(mm_fault(&m, t, 0x401, MM_ACCESS_READ) == 1);
    JT_CHECK(seen(0x401) != NULL && seen(0x401)[0] == 0x12u && (pte(0x401) & JOIST_PTE_W) != 0);
    JT_CHECK(mm_fault(&m, t, 0x402, MM_ACCESS_WRITE) == 1);
    JT_CHECK(seen(0x402) != NULL && seen(0x402)[0] == 0x13u);
    /* Another thread's fault on a page mapped since: resumed. */
    JT_CHECK(mm_fault(&m, t, 0x401, MM_ACCESS_READ) == 1);
    JT_CHECK(m.nfree == free_before && clean());
}

JT_TEST(mm_none_pages_kept_across_a_clone_then_copied_on_write)
{
    setup();
    t_with_pages();
    JT_REQUIRE(mm_protect(&m, t, 0x400, 4, MM_NONE) == MM_OK);
    uint32_t free_before = m.nfree;
    JT_REQUIRE(mm_clone(&m, t, c) == MM_OK);
    JT_CHECK(pte_in(C, 0x400) == 0 && m.task[c].nmap == 4 && m.nfree == free_before);
    JT_CHECK(mm_fault(&m, c, 0x400, MM_ACCESS_READ) == 0);
    JT_CHECK(clean());
    /* The child makes them accessible: shared until written. */
    JT_REQUIRE(mm_protect(&m, c, 0x400, 4, MM_W) == MM_OK);
    JT_CHECK(mm_fault(&m, c, 0x401, MM_ACCESS_WRITE) == 1); /* back, read-only: shared */
    JT_CHECK(seen_in(C, 0x401)[0] == 0x12u && (pte_in(C, 0x401) & JOIST_PTE_W) == 0);
    JT_CHECK(mm_fault(&m, c, 0x401, MM_ACCESS_WRITE | MM_ACCESS_PRESENT) == 1); /* copied */
    JT_CHECK(m.nfree == free_before - 1u && (pte_in(C, 0x401) & JOIST_PTE_W) != 0);
    seen_in(C, 0x401)[0] = 0x77u;
    /* The parent's is as it was; and SHARE copies a COW page the kernel
     * doesn't map (the parent's range made accessible, never faulted). */
    JT_REQUIRE(mm_protect(&m, t, 0x400, 4, MM_W) == MM_OK);
    JT_CHECK(pte(0x400) == 0);
    JT_CHECK(mm_share(&m, t, 0x400, 2, d, 0x900) == MM_OK);
    JT_CHECK(seen(0x400) != NULL && seen(0x400)[0] == 0x11u);
    JT_CHECK(seen_in(D, 0x900)[0] == 0x11u && seen_in(D, 0x901)[0] == 0x12u);
    JT_CHECK(seen_in(C, 0x401)[0] == 0x77u);
    JT_CHECK(pte(0x401) == 0); /* its last reference: nothing to copy, mapped at its fault */
    JT_CHECK(mm_fault(&m, t, 0x401, MM_ACCESS_READ) == 1 && seen(0x401)[0] == 0x12u);
    JT_CHECK(clean());
}

/* ---- Phase 5 step 3: region capabilities (0030) ---- */

/* A region of one table at LO in T, its capability in T's slot RS. */
#define LO (4u * JOIST_PT_ENTRIES)
#define RS 9u

JT_TEST(mm_region_new_gives_the_task_map_alone)
{
    setup();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 8, MM_W) == MM_OK);
    /* Refusals: a range off a table, no tables or too many, an unknown
     * permission, over anonymous memory; nothing made. */
    JT_CHECK(mm_region_new(&m, t, LO + 1u, 1, MM_W, RS) == MM_E_RANGE);
    JT_CHECK(mm_region_new(&m, t, LO, 0, MM_W, RS) == MM_E_RANGE);
    JT_CHECK(mm_region_new(&m, t, LO, MM_REGION_TABLES + 1u, MM_W, RS) == MM_E_RANGE);
    JT_CHECK(mm_region_new(&m, t, LO, 1, MM_NONE, RS) == MM_E_RANGE);
    JT_CHECK(mm_region_new(&m, t, 0, 4, MM_W, RS) == MM_E_STATE); /* meets 0x400 */
    JT_CHECK(mm_region_new(&m, 7, LO, 1, MM_W, RS) == MM_E_STATE); /* no such task */
    JT_CHECK(k.cs[T].slot[RS].obj == 0 && k.cs[root].slot[KREG].obj == 0);
    JT_REQUIRE(mm_region_new(&m, t, LO, 1, MM_W, RS) == MM_OK);
    const struct joist_cap *rc = joist__slot_c(&k, T, RS);
    JT_CHECK(rc != NULL && rc->type == JOIST_OBJ_REGION && rc->rights == JOIST_R_MAP);
    /* The same slot again, a range meeting it, the slot taken in T. */
    JT_CHECK(mm_region_new(&m, t, LO + JOIST_PT_ENTRIES, 1, MM_W, RS) == MM_E_STATE);
    JT_CHECK(mm_region_new(&m, t, LO, 1, MM_W, RS + 1u) == MM_E_STATE);
    JT_REQUIRE(joist_obj_create(&k, T, JOIST_OBJ_ENDPOINT, RS + 2u) == JOIST_OK);
    JT_CHECK(mm_region_new(&m, t, LO + JOIST_PT_ENTRIES, 1, MM_W, RS + 2u) == MM_E_STATE);
    JT_CHECK(k.cs[root].slot[KREG + 1u].obj == 0); /* the region it made, gone */
    /* Anonymous memory can't be asked for in it. */
    JT_CHECK(mm_map_anon(&m, t, LO + 4u, 2, MM_W) == MM_E_STATE);
    /* T emptied the slot itself: mm still has the region by that slot. */
    JT_REQUIRE(joist_cap_delete(&k, T, RS) == JOIST_OK);
    JT_CHECK(mm_region_new(&m, t, LO + 2u * JOIST_PT_ENTRIES, 1, MM_W, RS) == MM_E_STATE);
    JT_CHECK(clean());
}

JT_TEST(mm_region_grow_gives_zeroed_runs_the_task_maps_itself)
{
    setup();
    JT_REQUIRE(mm_region_new(&m, t, LO, 1, MM_W, RS) == MM_OK);
    uint32_t x0 = 9;
    uint32_t x1 = 9;
    JT_REQUIRE(mm_region_grow(&m, t, RS, 4, &x0) == MM_OK && x0 == 0);
    JT_REQUIRE(mm_region_grow(&m, t, RS, 3, &x1) == MM_OK && x1 == 1);
    JT_CHECK(m.nfree == POOL_PAGES - 7u && m.task[t].rpages == 7u);
    /* T maps them, by its own capability, without asking mm. */
    JT_CHECK(joist_region_map(&k, T, RS, 0, 0, 4, LO, JOIST_MAP_W) == JOIST_OK);
    JT_CHECK(joist_region_map(&k, T, RS, 1, 1, 2, LO + 100u, 0) == JOIST_OK);
    for (unsigned i = 0; i < 4; i++) {
        uint8_t *b = seen(LO + i);
        JT_CHECK(b != NULL && b[0] == 0 && b[JOIST_PAGE_SIZE - 1u] == 0); /* zeroed, not 0xa5 */
    }
    JT_CHECK(seen(LO + 100u) != NULL && seen(LO + 100u)[7] == 0);
    /* MAP alone: T can't grow it itself, nor map outside it. */
    JT_CHECK(joist_region_add(&k, T, RS, RS, 0, 1, &x1) != JOIST_OK);
    JT_CHECK(joist_region_map(&k, T, RS, 0, 0, 1, LO + JOIST_PT_ENTRIES, 0) == JOIST_E_RANGE);
    /* Refusals: no pages, more than the pool has, past 8 extents, an
     * unknown slot. */
    JT_CHECK(mm_region_grow(&m, t, RS, 0, &x1) == MM_E_RANGE);
    JT_CHECK(mm_region_grow(&m, t, RS, POOL_PAGES, &x1) == MM_E_NOMEM);
    JT_CHECK(mm_region_grow(&m, t, RS + 1u, 1, &x1) == MM_E_STATE);
    for (unsigned i = 2; i < MM_REGION_EXTENTS; i++) {
        JT_REQUIRE(mm_region_grow(&m, t, RS, 1, &x1) == MM_OK && x1 == i);
    }
    JT_CHECK(mm_region_grow(&m, t, RS, 1, &x1) == MM_E_NOMEM);
    JT_CHECK(clean());
}

JT_TEST(mm_region_grow_needs_a_run_in_one_frame)
{
    setup();
    /* The pool in pieces: all of it taken by anonymous memory, then two
     * pages apart given back. */
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, POOL_PAGES, MM_W) == MM_OK);
    for (uint64_t i = 0; i < POOL_PAGES; i++) {
        JT_REQUIRE(mm_fault(&m, t, 0x400 + i, MM_ACCESS_WRITE) == 1);
    }
    JT_REQUIRE(mm_unmap(&m, t, 0x400 + 3u, 1) == MM_OK);
    JT_REQUIRE(mm_unmap(&m, t, 0x400 + 7u, 1) == MM_OK);
    JT_REQUIRE(mm_region_new(&m, t, LO, 1, MM_W, RS) == MM_OK);
    uint32_t x;
    JT_CHECK(m.nfree == 2u && mm_region_grow(&m, t, RS, 2, &x) == MM_E_NOMEM);
    JT_REQUIRE(mm_region_grow(&m, t, RS, 1, &x) == MM_OK);
    JT_CHECK(clean());
}

JT_TEST(mm_region_grow_runs_stay_in_one_frame)
{
    /* The pool as two frames of 24 pages: no run of 30 in either. */
    setup();
    two_frames(24u);
    m.kreg_slot = KREG;
    JT_REQUIRE(mm_region_new(&m, t, LO, 1, MM_W, RS) == MM_OK);
    uint32_t x;
    JT_CHECK(mm_region_grow(&m, t, RS, 30, &x) == MM_E_NOMEM && m.nfree == POOL_PAGES);
    JT_CHECK(mm_region_grow(&m, t, RS, 24, &x) == MM_OK && mm_region_grow(&m, t, RS, 24, &x) == MM_OK);
    JT_CHECK(clean());
}

JT_TEST(mm_region_free_unmaps_then_gives_the_pages_back)
{
    setup();
    JT_REQUIRE(mm_region_new(&m, t, LO, 1, MM_W, RS) == MM_OK);
    uint32_t x;
    JT_REQUIRE(mm_region_grow(&m, t, RS, 4, &x) == MM_OK);
    JT_REQUIRE(joist_region_map(&k, T, RS, 0, 0, 4, LO, JOIST_MAP_W) == JOIST_OK);
    k.preempt = 1; /* the unmapping on the work list, a step at a time */
    JT_CHECK(mm_region_free(&m, t, RS) == MM_OK);
    JT_CHECK(pte(LO) == 0 && m.nfree == POOL_PAGES && m.task[t].rpages == 0);
    JT_CHECK(joist_region_map(&k, T, RS, 0, 0, 1, LO, 0) == JOIST_E_GONE);
    JT_CHECK(mm_region_free(&m, t, RS) == MM_E_STATE);
    /* T deletes its dead capability; the slot takes a new region. */
    JT_REQUIRE(joist_cap_delete(&k, T, RS) == JOIST_OK);
    JT_CHECK(mm_region_new(&m, t, LO, 1, MM_W, RS) == MM_OK);
    JT_CHECK(clean());
}

JT_TEST(mm_region_free_keeps_pages_whose_unmapping_did_not_finish)
{
    setup();
    JT_REQUIRE(mm_region_new(&m, t, LO, 2, MM_W, RS) == MM_OK);
    uint32_t x;
    JT_REQUIRE(mm_region_grow(&m, t, RS, 4, &x) == MM_OK);
    JT_REQUIRE(joist_region_map(&k, T, RS, 0, 0, 2, LO, JOIST_MAP_W) == JOIST_OK);
    JT_REQUIRE(joist_region_map(&k, T, RS, 0, 2, 2, LO + JOIST_PT_ENTRIES, JOIST_MAP_W) == JOIST_OK);
    k.preempt = 1;
    finish_steps = 0; /* mm gives up at once */
    JT_CHECK(mm_region_free(&m, t, RS) == MM_OK);
    /* Its pages stay out of the pool, though the record is no longer T's. */
    JT_CHECK(m.nfree == POOL_PAGES - 4u && m.task[t].rpages == 0 && mm_region_free(&m, t, RS) == MM_E_STATE);
    JT_CHECK(clean());
    finish_steps = 1000u;
    JT_CHECK(k_finish(NULL) == 1 && pte(LO) == 0 && pte(LO + JOIST_PT_ENTRIES) == 0);
    JT_CHECK(m.nfree == POOL_PAGES - 4u); /* lost: never handed out again */
    JT_CHECK(clean());
}

JT_TEST(mm_region_pages_come_back_with_the_task)
{
    setup();
    JT_REQUIRE(mm_region_new(&m, t, LO, 1, MM_W, RS) == MM_OK);
    uint32_t x;
    JT_REQUIRE(mm_region_grow(&m, t, RS, 5, &x) == MM_OK);
    JT_REQUIRE(joist_region_map(&k, T, RS, 0, 0, 5, LO, JOIST_MAP_W) == JOIST_OK);
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK);
    JT_REQUIRE(k_finish(NULL) == 1);
    mm_task_gone(&m, t);
    JT_CHECK(m.nfree == POOL_PAGES && !m.kreg[0].live && k.cs[root].slot[KREG].obj == 0);
    JT_CHECK(clean());
}

JT_TEST(mm_clone_copies_region_pages_and_maps_them_alike)
{
    setup();
    more_tasks();
    JT_REQUIRE(mm_region_new(&m, t, LO, 2, MM_W | MM_X, RS) == MM_OK);
    uint32_t x;
    JT_REQUIRE(mm_region_grow(&m, t, RS, 3, &x) == MM_OK);
    JT_REQUIRE(mm_region_grow(&m, t, RS, 2, &x) == MM_OK);
    /* Extent 0's pages 0-2 at LO (writable), extent 1's page 1 at LO + 9
     * (executable) and its page 0 in the second table (read-only). */
    JT_REQUIRE(joist_region_map(&k, T, RS, 0, 0, 3, LO, JOIST_MAP_W) == JOIST_OK);
    JT_REQUIRE(joist_region_map(&k, T, RS, 1, 1, 1, LO + 9u, JOIST_MAP_X) == JOIST_OK);
    JT_REQUIRE(joist_region_map(&k, T, RS, 1, 0, 1, LO + JOIST_PT_ENTRIES + 3u, 0) == JOIST_OK);
    /* Extent 0's page 0 and then extent 1's page 1, side by side: not one
     * run, though the pages' numbers follow on. */
    JT_REQUIRE(joist_region_map(&k, T, RS, 0, 0, 1, LO + 20u, 0) == JOIST_OK);
    JT_REQUIRE(joist_region_map(&k, T, RS, 1, 1, 1, LO + 21u, 0) == JOIST_OK);
    seen(LO)[0] = 0x41;
    seen(LO + 2u)[5] = 0x42;
    seen(LO + 9u)[0] = 0x43;
    seen(LO + JOIST_PT_ENTRIES + 3u)[1] = 0x44;
    JT_REQUIRE(mm_clone(&m, t, c) == MM_OK);
    /* C holds its own region at the same slot, MAP alone. */
    const struct joist_cap *cc = joist__slot_c(&k, C, RS);
    JT_CHECK(cc != NULL && cc->type == JOIST_OBJ_REGION && cc->rights == JOIST_R_MAP &&
             cc->obj != joist__slot_c(&k, T, RS)->obj);
    JT_CHECK(m.task[c].rpages == 5u && m.nfree == POOL_PAGES - 10u);
    /* The same pages mapped, with the same permissions, holding copies on
     * pages of their own (never shared: 0030). */
    uint64_t at[] = {LO, LO + 1u, LO + 2u, LO + 9u, LO + 20u, LO + 21u, LO + JOIST_PT_ENTRIES + 3u};
    for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++) {
        uint64_t pt = pte_in(T, at[i]);
        uint64_t pc = pte_in(C, at[i]);
        JT_CHECK(pc != 0 && seen_in(C, at[i]) != seen(at[i]));
        JT_CHECK(JOIST_PTE_PERMS(pt) == JOIST_PTE_PERMS(pc));
        JT_CHECK(memcmp(seen_in(C, at[i]), seen(at[i]), JOIST_PAGE_SIZE) == 0);
    }
    JT_CHECK(pte_in(C, LO + 3u) == 0 && pte_in(C, LO + 8u) == 0);
    JT_CHECK(seen_in(C, LO)[0] == 0x41 && seen_in(C, LO + JOIST_PT_ENTRIES + 3u)[1] == 0x44);
    seen_in(C, LO)[0] = 0x51;
    JT_CHECK(seen(LO)[0] == 0x41);
    /* C maps its own copy by its own capability. */
    JT_CHECK(joist_region_map(&k, C, RS, 0, 1, 1, LO + 30u, 0) == JOIST_OK);
    JT_CHECK(joist_region_map(&k, C, RS, 0, 0, 1, LO, 0) == JOIST_E_OCCUPIED);
    /* No anonymous memory or share lands in either's region. */
    JT_CHECK(mm_map_anon(&m, c, LO, 1, MM_W) == MM_E_STATE);
    JT_REQUIRE(mm_map_anon(&m, d, 0x400, 4, MM_W) == MM_OK);
    JT_CHECK(mm_share(&m, d, 0x400, 2, t, LO + 40u) == MM_E_STATE);
    JT_CHECK(clean());
}


/* ---- Phase 5 step 4: trees, their reservations and ceilings (0028) ---- */

/* A kernel task in root's slot ts, its tables paid from frame mem. */
static int task_made(uint32_t ts, uint32_t memf)
{
    return joist_task_create(&k, root, 20, ts) == JOIST_OK && joist_space_donate(&k, root, ts, memf) == JOIST_OK;
}

static uint32_t tree_of(uint32_t x)
{
    return m.task[x].tree;
}

JT_TEST(mm_tree_ceiling_holds_on_every_path_that_makes_a_page)
{
    setup();
    more_tasks(); /* C in T's tree, D a tree of its own */
    JT_REQUIRE(mm_set_limit(&m, t, 0, 6, MM_MAX_TASKS) == MM_OK);
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 16, MM_W) == MM_OK);
    /* Demand-zero: six pages, then the ceiling. */
    for (uint64_t i = 0; i < 6; i++) {
        JT_REQUIRE(mm_fault(&m, t, 0x400 + i, MM_ACCESS_WRITE) == 1);
    }
    JT_CHECK(mm_fault(&m, t, 0x406, MM_ACCESS_WRITE) == 0 && m.tree[tree_of(t)].pages == 6u);
    JT_CHECK(mm_fault(&m, t, 0x406, MM_ACCESS_READ) == 0);
    /* A copy on write in C (the same tree): refused at the ceiling, made
     * once a page is given back. */
    JT_REQUIRE(mm_clone(&m, t, c) == MM_OK);
    JT_CHECK(mm_fault(&m, c, 0x400, MM_ACCESS_WRITE | MM_ACCESS_PRESENT) == 0);
    JT_REQUIRE(mm_unmap(&m, t, 0x405, 1) == MM_OK && mm_unmap(&m, c, 0x405, 1) == MM_OK);
    JT_CHECK(m.tree[tree_of(t)].pages == 5u);
    JT_CHECK(mm_fault(&m, c, 0x400, MM_ACCESS_WRITE | MM_ACCESS_PRESENT) == 1);
    JT_CHECK(m.tree[tree_of(t)].pages == 6u);
    /* SHARE that must make a page: refused before it changes anything. */
    struct mm_region before[MM_REGIONS];
    memcpy(before, m.task[t].reg, sizeof before);
    JT_CHECK(mm_share(&m, t, 0x408, 2, d, 0x900) == MM_E_NOMEM);
    JT_CHECK(memcmp(before, m.task[t].reg, sizeof before) == 0 && m.task[d].reg[0].n == 0);
    /* A region's extent: refused past the ceiling. */
    JT_REQUIRE(mm_region_new(&m, t, LO, 1, MM_W, RS) == MM_OK);
    uint32_t x;
    JT_CHECK(mm_region_grow(&m, t, RS, 1, &x) == MM_E_NOMEM);
    /* D, another tree, isn't held back by T's ceiling. */
    JT_REQUIRE(mm_map_anon(&m, d, 0x400, 8, MM_W) == MM_OK);
    JT_CHECK(mm_fault(&m, d, 0x400, MM_ACCESS_READ) == 1 && m.tree[tree_of(d)].pages > 6u);
    JT_CHECK(clean());
}

JT_TEST(mm_tree_reservation_is_kept_for_it)
{
    setup();
    more_tasks();
    JT_REQUIRE(mm_set_limit(&m, d, 10, POOL_PAGES, MM_MAX_TASKS) == MM_OK);
    /* T may take all but D's 10. */
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, POOL_PAGES, MM_W) == MM_OK);
    for (uint64_t i = 0; i < POOL_PAGES - 10u; i++) {
        JT_REQUIRE(mm_fault(&m, t, 0x400 + i, MM_ACCESS_WRITE) == 1);
    }
    JT_CHECK(mm_fault(&m, t, 0x400 + POOL_PAGES - 10u, MM_ACCESS_WRITE) == 0 && m.nfree == 10u);
    /* D gets its 10, past them nothing (the pool is empty). */
    JT_REQUIRE(mm_map_anon(&m, d, 0x400, 16, MM_W) == MM_OK);
    for (uint64_t i = 0; i < 10; i++) {
        JT_CHECK(mm_fault(&m, d, 0x400 + i, MM_ACCESS_WRITE) == 1);
    }
    JT_CHECK(mm_fault(&m, d, 0x40a, MM_ACCESS_WRITE) == 0 && m.nfree == 0);
    JT_CHECK(clean());
}

JT_TEST(mm_tree_admission_and_limits_refused_when_reservations_dont_fit)
{
    setup();
    JT_REQUIRE(mm_set_limit(&m, t, 40, POOL_PAGES, MM_MAX_TASKS) == MM_OK);
    JT_CHECK(mm_set_limit(&m, t, POOL_PAGES + 1u, POOL_PAGES, MM_MAX_TASKS) == MM_E_NOMEM);
    JT_REQUIRE(task_made(TD, MEMD));
    /* 8 pages left unreserved: the default reservation (32) doesn't fit. */
    JT_CHECK(mm_task_add(&m, TD, &d) == MM_E_NOMEM);
    JT_CHECK(m.tree[1].live == 0 && !m.task[1].live);
    JT_REQUIRE(mm_set_limit(&m, t, POOL_PAGES - MM_TREE_RESERVE, POOL_PAGES, MM_MAX_TASKS) == MM_OK);
    JT_REQUIRE(mm_task_add(&m, TD, &d) == MM_OK);
    JT_CHECK(m.tree[tree_of(d)].reserve == MM_TREE_RESERVE && m.tree[tree_of(d)].ceil == POOL_PAGES / 2u &&
             m.tree[tree_of(d)].ceil_tasks == MM_MAX_TASKS / 2u);
    /* Every page reserved now: neither may grow its reservation. */
    JT_CHECK(mm_set_limit(&m, d, MM_TREE_RESERVE + 1u, POOL_PAGES, MM_MAX_TASKS) == MM_E_NOMEM);
    JT_CHECK(clean());
}

JT_TEST(mm_tree_tasks_children_counted_kept_and_given_back)
{
    setup();
    JT_REQUIRE(mm_set_limit(&m, t, 4, POOL_PAGES, 2) == MM_OK);
    uint32_t r = tree_of(t);
    JT_REQUIRE(task_made(TC, MEMC) && task_made(TD, MEMD));
    /* A child counts from the moment it is made (pending, 0023): a second
     * is past the task ceiling of 2. */
    JT_REQUIRE(mm_task_add_child(&m, TC, t, &c) == MM_OK);
    JT_CHECK(tree_of(c) == r && m.tree[r].tasks == 2u);
    JT_CHECK(mm_task_add_child(&m, TD, t, &d) == MM_E_NOMEM && m.tree[r].tasks == 2u);
    /* A clone needs its child in its tree. */
    JT_REQUIRE(mm_task_add(&m, TD, &d) == MM_OK);
    JT_CHECK(mm_clone(&m, t, d) == MM_E_STATE);
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 4, MM_W) == MM_OK && mm_fault(&m, t, 0x400, MM_ACCESS_WRITE) == 1);
    JT_REQUIRE(mm_clone(&m, t, c) == MM_OK);
    /* The parent goes: the child stays in the tree, which keeps its pages
     * (the shared one) and its limits. */
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK && k_finish(NULL) == 1);
    mm_task_gone(&m, t);
    JT_CHECK(m.tree[r].live && m.tree[r].tasks == 1u && m.tree[r].pages == 1u && m.tree[r].reserve == 4u);
    JT_CHECK(mm_fault(&m, c, 0x401, MM_ACCESS_WRITE) == 1 && m.tree[r].pages == 2u);
    JT_CHECK(clean());
    /* The last one goes: the tree with it, and everything comes back. */
    JT_REQUIRE(joist_task_destroy(&k, root, TC) == JOIST_OK && k_finish(NULL) == 1);
    mm_task_gone(&m, c);
    JT_CHECK(!m.tree[r].live && m.nfree == POOL_PAGES);
    JT_CHECK(clean());
}

JT_TEST(mm_tree_keeps_pages_it_shared_after_its_tasks_go)
{
    setup();
    more_tasks();
    JT_REQUIRE(mm_map_anon(&m, t, 0x400, 4, MM_W) == MM_OK && mm_fault(&m, t, 0x400, MM_ACCESS_WRITE) == 1);
    JT_REQUIRE(mm_share(&m, t, 0x400, 2, d, 0x900) == MM_OK); /* T makes the second page */
    uint32_t r = tree_of(t);
    JT_REQUIRE(mm_set_limit(&m, t, 5, POOL_PAGES, MM_MAX_TASKS) == MM_OK);
    JT_CHECK(m.tree[r].pages == 2u && m.tree[tree_of(d)].pages == 0);
    /* T and its child C go: T's tree stays while D maps its pages, with no
     * reservation; it goes when D lets them go. */
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK && joist_task_destroy(&k, root, TC) == JOIST_OK &&
               k_finish(NULL) == 1);
    mm_task_gone(&m, t);
    mm_task_gone(&m, c);
    JT_CHECK(m.tree[r].live && m.tree[r].tasks == 0 && m.tree[r].reserve == 0 && m.tree[r].pages == 2u);
    JT_CHECK(clean());
    JT_REQUIRE(mm_unmap(&m, d, 0x900, 2) == MM_OK);
    JT_CHECK(!m.tree[r].live && m.nfree == POOL_PAGES);
    JT_CHECK(clean());
}

/* ---- page tables on demand (0021; the owner's design of 2026-11-10) ---- */

/* E, served by mm in a tree of its own, its page tables paid for with 6
 * lent pages (MEME): the first page it maps takes them all (a table and
 * its shadow at each level below the top), each new 2 MiB span after it
 * two more. GROWF holds `runs` runs to lend on demand. */
static uint32_t e;
static joist_cspace E;

static void grow_world(unsigned runs)
{
    grow_on = 1;
    grow_left = (uint64_t)runs * MM_TABLE_GROW + 1u;
    if (joist_boot_frame(&k, root, MEME, MEM_BASE + 96u, 6) != JOIST_OK ||
        joist_boot_frame(&k, root, GROWF, MEM_BASE + 102u, grow_left) != JOIST_OK ||
        joist_task_create(&k, root, 20, TE) != JOIST_OK ||
        joist_space_donate(&k, root, TE, MEME) != JOIST_OK || mm_task_add(&m, TE, &e) != MM_OK ||
        mm_set_limit(&m, e, 0, POOL_PAGES + 4u * MM_TABLE_GROWS * MM_TABLE_GROW, MM_MAX_TASKS) != MM_OK ||
        mm_map_anon(&m, e, 0x400, 48u * JOIST_PT_ENTRIES, MM_W) != MM_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    E = k.obj[k.cs[root].slot[TE].obj - 1u].target - 1u;
}

/* A write fault in E's span i (one page): mm's answer. */
static int span_fault(unsigned i)
{
    return mm_fault(&m, e, 0x400 + (uint64_t)i * JOIST_PT_ENTRIES, MM_ACCESS_WRITE);
}

JT_TEST(mm_tables_grow_on_demand)
{
    setup();
    grow_world(3);
    JT_CHECK(span_fault(0) == 1 && m.task[e].ngrow == 0 && donates == 0); /* the 6 lent pages */
    /* Each new span takes two pages: a run of 16 lasts eight spans. */
    for (unsigned i = 1; i <= 8; i++) {
        JT_CHECK(span_fault(i) == 1);
    }
    JT_CHECK(m.task[e].ngrow == 1 && donates == 1 &&
             m.tree[m.task[e].tree].tables == MM_TABLE_GROW);
    JT_CHECK(span_fault(9) == 1 && m.task[e].ngrow == 2 &&
             m.tree[m.task[e].tree].tables == 2u * MM_TABLE_GROW);
    for (unsigned i = 0; i <= 9; i++) {
        JT_CHECK(pte_in(E, 0x400 + (uint64_t)i * JOIST_PT_ENTRIES) != 0);
    }
    JT_CHECK(clean());
}

JT_TEST(mm_tables_refused_when_none_is_left_changes_nothing)
{
    setup();
    grow_world(1);
    for (unsigned i = 0; i <= 8; i++) {
        JT_REQUIRE(span_fault(i) == 1);
    }
    JT_REQUIRE(m.task[e].ngrow == 1);
    uint32_t nmap = m.task[e].nmap, nfree = m.nfree;
    /* No run left: the fault is refused, and nothing has changed. */
    JT_CHECK(span_fault(9) == 0 && donates == 2);
    JT_CHECK(m.task[e].ngrow == 1 && m.tree[m.task[e].tree].tables == MM_TABLE_GROW &&
             m.task[e].nmap == nmap && m.nfree == nfree &&
             pte_in(E, 0x400 + 9u * JOIST_PT_ENTRIES) == 0);
    JT_CHECK(clean());
}

JT_TEST(mm_tables_held_to_the_tree_ceiling)
{
    setup();
    grow_world(3);
    JT_REQUIRE(span_fault(0) == 1);
    /* Room for one more page but not a run: no run is asked for. */
    const struct mm_tree *g = &m.tree[m.task[e].tree];
    JT_REQUIRE(mm_set_limit(&m, e, 0, g->pages + g->tables + MM_TABLE_GROW - 1u, MM_MAX_TASKS) == MM_OK);
    JT_CHECK(span_fault(1) == 0 && donates == 0 && m.task[e].ngrow == 0 && g->tables == 0);
    /* With room for it, it is lent. */
    JT_REQUIRE(mm_set_limit(&m, e, 0, g->pages + g->tables + MM_TABLE_GROW + 1u, MM_MAX_TASKS) == MM_OK);
    JT_CHECK(span_fault(1) == 1 && m.task[e].ngrow == 1 && g->tables == MM_TABLE_GROW);
    JT_CHECK(clean());
}

JT_TEST(mm_tables_at_most_MM_TABLE_GROWS_a_task)
{
    setup();
    grow_world(MM_TABLE_GROWS + 1u);
    unsigned i = 0;
    while (i < 1u + 8u * MM_TABLE_GROWS) {
        JT_REQUIRE(span_fault(i) == 1);
        i++;
    }
    JT_CHECK(m.task[e].ngrow == MM_TABLE_GROWS && donates == MM_TABLE_GROWS);
    /* A run is left, but the task has its most. */
    JT_CHECK(span_fault(i) == 0 && donates == MM_TABLE_GROWS && m.task[e].ngrow == MM_TABLE_GROWS);
    JT_CHECK(clean());
}

JT_TEST(mm_tables_come_back_when_the_task_goes)
{
    setup();
    grow_world(3);
    for (unsigned i = 0; i <= 9; i++) {
        JT_REQUIRE(span_fault(i) == 1);
    }
    JT_REQUIRE(m.task[e].ngrow == 2);
    uint32_t r = m.task[e].tree;
    JT_REQUIRE(joist_task_destroy(&k, root, TE) == JOIST_OK);
    mm_task_gone(&m, e);
    JT_CHECK(undonates == 2 && nback == 2 && !m.tree[r].live && m.tree[r].tables == 0);
    JT_CHECK(m.nfree == POOL_PAGES && clean());
    /* Lent again: the kernel let them go with E's space. */
    JT_REQUIRE(joist_cap_delete(&k, root, TE) == JOIST_OK && joist_task_create(&k, root, 20, TE) == JOIST_OK);
    uint32_t run = 0;
    JT_CHECK(k_donate(NULL, TE, &run) == 0 && (run == GROW0 || run == GROW0 + 1u) && nback == 1);
}
