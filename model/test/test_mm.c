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
static joist_cspace root, T;
static struct mm m;
static uint32_t t;

enum { TK = 0, MEM = 1, SELF = 2, POOL = 3 }; /* root's slots */

/* Memory for T's page tables (lent pages), as test_map.c. */
#define MEM_BASE 0x4000u
#define MEM_PAGES 32u
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

static int k_map(void *ctx, uint32_t fs, uint64_t off, uint64_t n, uint32_t ts, uint64_t va,
                 unsigned perms)
{
    (void)ctx;
    return joist_map(&k, root, fs, off, n, ts, va, perms) != JOIST_OK;
}

static int k_map_replace(void *ctx, uint32_t fs, uint64_t off, uint32_t ts, uint64_t va,
                         unsigned perms, uint32_t ofs, uint64_t ooff)
{
    (void)ctx;
    return joist_map_replace(&k, root, fs, off, 1, ts, va, perms, ofs, ooff) != JOIST_OK;
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

static uint8_t *k_page(void *ctx, uint32_t fs, uint64_t off)
{
    (void)ctx;
    return fs == POOL && off < POOL_PAGES ? pool[off] : NULL;
}

static const struct mm_kops kops = {
    .ctx = NULL, .map = k_map, .map_replace = k_map_replace, .unmap = k_unmap,
    .protect = k_protect, .page = k_page};

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
    memset(mem, 0, sizeof mem);
    memset(pool, 0xa5, sizeof pool); /* a page mm hands out must be zeroed */
    k.hooks.page = page;
    if (joist_boot(&k, 64, &root) != JOIST_OK ||
        joist_boot_frame(&k, root, MEM, MEM_BASE, MEM_PAGES) != JOIST_OK ||
        joist_boot_frame(&k, root, POOL, POOL_BASE, POOL_PAGES) != JOIST_OK ||
        joist_boot_self(&k, root, SELF) != JOIST_OK || joist_task_create(&k, root, 20, TK) != JOIST_OK ||
        joist_space_donate(&k, root, TK, MEM) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    T = k.obj[k.cs[root].slot[TK].obj - 1u].target - 1u;
    mm_init(&m, &kops);
    if (mm_add_frame(&m, POOL, POOL_PAGES) != MM_OK || mm_task_add(&m, TK, &t) != MM_OK) {
        jt_fail(__FILE__, __LINE__, "mm could not be set up as written", 0);
    }
}

/* The kernel's entry for page va of T, 0 if none. */
static uint64_t pte(uint64_t va)
{
    uint64_t e = k.cs[T].pt_top[va >> JOIST_PT_SHIFT(3)];
    for (unsigned l = 2; e != 0 && l > 0; l--) {
        e = ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[(va >> JOIST_PT_SHIFT(l)) &
                                                                (JOIST_PT_ENTRIES - 1u)];
    }
    return e == 0 ? 0 : ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[va & (JOIST_PT_ENTRIES - 1u)];
}

/* The bytes T sees at page va, through the kernel's entry: NULL if unmapped. */
static uint8_t *seen(uint64_t va)
{
    uint64_t e = pte(va);
    if (e == 0) {
        return NULL;
    }
    const struct joist_object *f = &k.obj[JOIST_PTE_FRAME(e)];
    uint64_t pfn = f->base + JOIST_PTE_OFF(e);
    return pfn >= POOL_BASE && pfn < POOL_BASE + POOL_PAGES ? pool[pfn - POOL_BASE] : NULL;
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
    JT_CHECK(mm_map_anon(&m, t, 0x408, 1, 0x4u) == MM_E_RANGE);
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
