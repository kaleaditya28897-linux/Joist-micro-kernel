/* SPDX-License-Identifier: GPL-3.0-only */
/* test_mm_refine.c - mm, refined (decision 0023; the plan's Testing: "COW,
 * refined"; Phase 4 step 6, part C).
 *
 * An abstract machine, in which every task owns pages that hold values, a
 * clone copies its private pages and shares its shared ones, and sharing
 * makes two tasks' pages one; against the concrete one: the kernel model,
 * mm's library, and the fault path (a read or write goes through the
 * kernel's page tables, and faults into mm when the page is not there or
 * not writable). Random sequences of map, read, write, unmap, protect,
 * share, clone and exit, seeded and replayable. After every step: every
 * value read must be the abstract one, mm's records and the kernel's
 * invariants must hold, and mm's records must cover the kernel's mappings
 * exactly (no page mapped that mm doesn't know of, none it knows of
 * missing, each naming the page mm says). */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"
#include "mmlib.h"

#include <stdlib.h>
#include <string.h>

/* ---- the concrete machine ---- */

static struct joist_kernel k;
static joist_cspace root;
static struct mm m;

#define NT 6u           /* task places */
#define WLO 0x580u      /* the window of pages the tasks use, across two */
#define WN 0x100u       /* tables' spans (the boundary at 0x600) */
#define MEM_BASE 0x4000u
#define MEM_EACH 32u    /* pages lent to each task for its tables */
#define POOL_BASE 0x9000u
#define POOL_PAGES 512u
#define TSLOT(i) (10u + (i))
#define MSLOT(i) (30u + (i))
#define POOL 3u

static uint64_t mem[NT * MEM_EACH][JOIST_PAGE_SIZE / 8u];
static uint8_t pool[POOL_PAGES][JOIST_PAGE_SIZE];

static void *page(void *ctx, uint64_t pfn)
{
    (void)ctx;
    return pfn >= MEM_BASE && pfn < MEM_BASE + NT * MEM_EACH ? mem[pfn - MEM_BASE] : NULL;
}

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

/* ---- the abstract machine ---- */

struct apage {
    uint32_t region; /* 0: in no region; else the region's id */
    uint8_t perms;
    uint8_t shared;
    uint32_t cell;   /* 0: never written (reads 0); else a value cell */
};

static struct {
    int live;
    uint32_t mt;     /* mm's index for it */
    joist_cspace s;  /* the kernel's space */
    struct apage pg[WN];
} at[NT];

#define NCELLS 200000u
static uint64_t cell[NCELLS];
static uint32_t ncells;
static uint32_t next_region;

static uint32_t cell_new(uint64_t v)
{
    if (ncells + 1u >= NCELLS) {
        return 0;
    }
    cell[++ncells] = v;
    return ncells;
}

/* ---- the world ---- */

static uint64_t rng;
static uint32_t pick(uint32_t n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng % n);
}

static int start_task(unsigned i)
{
    if (joist_task_create(&k, root, 20, TSLOT(i)) != JOIST_OK ||
        joist_space_donate(&k, root, TSLOT(i), MSLOT(i)) != JOIST_OK ||
        mm_task_add(&m, TSLOT(i), &at[i].mt) != MM_OK) {
        return 0;
    }
    at[i].live = 1;
    at[i].s = k.obj[k.cs[root].slot[TSLOT(i)].obj - 1u].target - 1u;
    memset(at[i].pg, 0, sizeof at[i].pg);
    return 1;
}

static int setup(void)
{
    joist_init(&k);
    memset(mem, 0, sizeof mem);
    memset(pool, 0xa5, sizeof pool);
    memset(at, 0, sizeof at);
    ncells = 0;
    next_region = 0;
    k.hooks.page = page;
    if (joist_boot(&k, 512, &root) != JOIST_OK ||
        joist_boot_frame(&k, root, POOL, POOL_BASE, POOL_PAGES) != JOIST_OK) {
        return 0;
    }
    for (unsigned i = 0; i < NT; i++) {
        if (joist_boot_frame(&k, root, MSLOT(i), MEM_BASE + i * MEM_EACH, MEM_EACH) != JOIST_OK) {
            return 0;
        }
    }
    mm_init(&m, &kops);
    return mm_add_frame(&m, POOL, POOL_PAGES) == MM_OK && start_task(0);
}

/* The kernel's entry for page va of space s, 0 if none. */
static uint64_t pte(joist_cspace s, uint64_t va)
{
    uint64_t e = k.cs[s].pt_top[va >> JOIST_PT_SHIFT(3)];
    for (unsigned l = 2; e != 0 && l > 0; l--) {
        e = ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[(va >> JOIST_PT_SHIFT(l)) &
                                                                (JOIST_PT_ENTRIES - 1u)];
    }
    return e == 0 ? 0 : ((const uint64_t *)page(NULL, JOIST_PT_SHADOW(e)))[va & (JOIST_PT_ENTRIES - 1u)];
}

static uint64_t *word(uint64_t e)
{
    const struct joist_object *f = &k.obj[JOIST_PTE_FRAME(e)];
    uint64_t pfn = f->base + JOIST_PTE_OFF(e);
    return pfn >= POOL_BASE && pfn < POOL_BASE + POOL_PAGES ? (uint64_t *)pool[pfn - POOL_BASE] : NULL;
}

/* ---- one step ---- */

static char why[240];

/* What the runs did, so a pass can't be a run of refusals. */
static struct {
    unsigned long reads_written; /* reads of a page holding a value written */
    unsigned long writes;
    unsigned long cow_copies;    /* write faults on a present page that took a new page */
    unsigned long clones, shares, unmaps, protects, exits, refused;
} cov;
#define WRONG(...)                                                                                 \
    do {                                                                                           \
        (void)snprintf(why, sizeof why, __VA_ARGS__);                                              \
        return 0;                                                                                  \
    } while (0)

static unsigned live_task(void)
{
    unsigned i = pick(NT);
    for (unsigned j = 0; j < NT && !at[(i + j) % NT].live; j++) {
        if (j + 1u == NT) {
            return NT;
        }
    }
    while (!at[i].live) {
        i = (i + 1u) % NT;
    }
    return i;
}

/* A page in the window, and a run from it, now and then across a span. */
static uint64_t pick_va(void)
{
    return pick(8) == 0 ? 0x5f8u + pick(16) - WLO : pick(WN);
}

/* Mostly a page of task i in a region (a writable one if w), so reads and
 * writes do more than fault and be refused; now and then any page. */
static uint64_t pick_used(unsigned i, int w)
{
    if (pick(4) != 0) {
        uint64_t start = pick(WN);
        for (uint64_t k2 = 0; k2 < WN; k2++) {
            const struct apage *p = &at[i].pg[(start + k2) % WN];
            if (p->region != 0 && (!w || (p->perms & MM_W) != 0)) {
                return (start + k2) % WN;
            }
        }
    }
    return pick(WN);
}

/* Read page a (an offset in the window) of task i: as the task would, a
 * fault into mm if the kernel has nothing there. */
static int do_read(unsigned i, uint64_t a)
{
    const struct apage *p = &at[i].pg[a];
    uint64_t va = WLO + a;
    uint64_t e = pte(at[i].s, va);
    if (e == 0) {
        int r = mm_fault(&m, at[i].mt, va, MM_ACCESS_READ);
        if (r != (p->region != 0)) {
            if (r == 0 && m.nfree == 0) {
                return 1; /* the pool spent: an honest refusal */
            }
            WRONG("read of task %u page 0x%llx: mm says %d, the region %u", i, (unsigned long long)va,
                  r, p->region);
        }
        if (r == 0) {
            return 1;
        }
        e = pte(at[i].s, va);
        if (e == 0) {
            WRONG("read of task %u page 0x%llx: resumed, but nothing is mapped", i, (unsigned long long)va);
        }
    } else if (p->region == 0) {
        WRONG("task %u page 0x%llx is mapped outside its regions", i, (unsigned long long)va);
    }
    uint64_t want = p->cell != 0 ? cell[p->cell] : 0;
    cov.reads_written += want != 0;
    uint64_t *w = word(e);
    if (w == NULL || *w != want) {
        WRONG("read of task %u page 0x%llx: 0x%llx, the abstract value 0x%llx", i, (unsigned long long)va,
              w != NULL ? (unsigned long long)*w : 0ull, (unsigned long long)want);
    }
    return 1;
}

static int do_write(unsigned i, uint64_t a, uint64_t v)
{
    struct apage *p = &at[i].pg[a];
    uint64_t va = WLO + a;
    int allowed = p->region != 0 && (p->perms & MM_W) != 0;
    uint64_t e = pte(at[i].s, va);
    if (e == 0 || (e & JOIST_PTE_W) == 0) {
        unsigned access = MM_ACCESS_WRITE | (e != 0 ? MM_ACCESS_PRESENT : 0u);
        uint32_t before = m.nfree;
        int r = mm_fault(&m, at[i].mt, va, access);
        cov.cow_copies += r == 1 && e != 0 && m.nfree < before;
        cov.refused += r == 0;
        if (r != allowed) {
            if (r == 0 && m.nfree == 0) {
                return 1;
            }
            WRONG("write to task %u page 0x%llx: mm says %d, allowed %d", i, (unsigned long long)va, r,
                  allowed);
        }
        if (r == 0) {
            return 1;
        }
        e = pte(at[i].s, va);
        if (e == 0 || (e & JOIST_PTE_W) == 0) {
            WRONG("write to task %u page 0x%llx: resumed, not writable", i, (unsigned long long)va);
        }
    } else if (!allowed) {
        WRONG("task %u page 0x%llx is writable where it may not be", i, (unsigned long long)va);
    }
    uint64_t *w = word(e);
    if (w == NULL) {
        WRONG("task %u page 0x%llx maps no pool page", i, (unsigned long long)va);
    }
    *w = v;
    cov.writes++;
    /* A page's cell is its own unless shared (a clone copies private
     * cells): a write sets it, or makes it. */
    if (p->cell == 0) {
        p->cell = cell_new(v);
    } else {
        cell[p->cell] = v;
    }
    return 1;
}

/* Pages past a range that kept region id `id`: a separate region now. */
static void renumber_after(unsigned i, uint64_t a, uint32_t id)
{
    uint32_t fresh = ++next_region;
    for (uint64_t b = a; b < WN && at[i].pg[b].region == id; b++) {
        at[i].pg[b].region = fresh;
    }
}

static int step(void)
{
    unsigned i = live_task();
    if (i == NT) {
        if (!start_task(0)) {
            WRONG("no task could be started");
        }
        return 1;
    }
    uint32_t t = at[i].mt;
    uint64_t a = pick_va();
    uint64_t n = 1u + pick(pick(4) == 0 ? 64u : 8u);
    if (a + n > WN) {
        n = WN - a;
    }
    /* Half writable, else read-only or executable; now and then W|X. */
    static const unsigned some[4] = {0, MM_W, MM_W, MM_X};
    unsigned perms = pick(16) == 0 ? (MM_W | MM_X) : some[pick(4)];
    switch (pick(16)) {
    case 0:
    case 1: { /* map anonymous memory */
        int clash = 0;
        for (uint64_t b = a; b < a + n; b++) {
            clash |= at[i].pg[b].region != 0;
        }
        int e = mm_map_anon(&m, t, WLO + a, n, perms);
        int want = perms == (MM_W | MM_X) ? MM_E_RANGE : clash ? MM_E_STATE : MM_OK;
        if (e != want && !(want == MM_OK && e == MM_E_NOMEM)) {
            WRONG("map_anon(task %u, 0x%llx, %llu, %u): %d, expected %d", i, (unsigned long long)(WLO + a),
                  (unsigned long long)n, perms, e, want);
        }
        if (e == MM_OK) {
            uint32_t id = ++next_region;
            for (uint64_t b = a; b < a + n; b++) {
                at[i].pg[b] = (struct apage){.region = id, .perms = (uint8_t)perms};
            }
        }
        return 1;
    }
    case 2:
    case 3:
    case 4:
    case 5:
        return do_read(i, pick_used(i, 0));
    case 6:
    case 7:
    case 8:
    case 9:
        return do_write(i, pick_used(i, 1), rng | 1u);
    case 10: { /* unmap */
        int e = mm_unmap(&m, t, WLO + a, n);
        if (e != MM_OK && e != MM_E_NOMEM) {
            WRONG("unmap(task %u, 0x%llx, %llu): %d", i, (unsigned long long)(WLO + a), (unsigned long long)n, e);
        }
        cov.unmaps += e == MM_OK;
        if (e == MM_OK) {
            uint32_t id = a + n < WN ? at[i].pg[a + n].region : 0;
            for (uint64_t b = a; b < a + n; b++) {
                at[i].pg[b] = (struct apage){0};
            }
            if (id != 0 && a > 0 && at[i].pg[a - 1u].region == id) {
                renumber_after(i, a + n, id); /* split in two */
            }
        }
        return 1;
    }
    case 11: { /* protect */
        int covered = 1;
        int mixed = 0;
        for (uint64_t b = a; b < a + n; b++) {
            covered &= at[i].pg[b].region != 0;
            mixed |= at[i].pg[b].shared != at[i].pg[a].shared;
        }
        int e = mm_protect(&m, t, WLO + a, n, perms);
        int want = perms == (MM_W | MM_X) ? MM_E_RANGE : !covered || mixed ? MM_E_STATE : MM_OK;
        if (e != want && !(want == MM_OK && e == MM_E_NOMEM)) {
            WRONG("protect(task %u, 0x%llx, %llu, %u): %d, expected %d", i, (unsigned long long)(WLO + a),
                  (unsigned long long)n, perms, e, want);
        }
        cov.protects += e == MM_OK;
        if (e == MM_OK) {
            uint32_t after = a + n < WN ? at[i].pg[a + n].region : 0;
            uint32_t id = ++next_region;
            for (uint64_t b = a; b < a + n; b++) {
                at[i].pg[b].region = id;
                at[i].pg[b].perms = (uint8_t)perms;
            }
            if (after != 0) {
                renumber_after(i, a + n, after); /* the cut's right side is a region of its own */
            }
        }
        return 1;
    }
    case 12: { /* clone into a free place */
        unsigned c = NT;
        for (unsigned j = 0; j < NT; j++) {
            if (!at[j].live) {
                c = j;
                break;
            }
        }
        if (c == NT || !start_task(c)) {
            return 1;
        }
        int e = mm_clone(&m, t, at[c].mt);
        if (e != MM_OK) {
            WRONG("clone(task %u into %u): %d", i, c, e);
        }
        cov.clones++;
        for (uint64_t b = 0; b < WN; b++) {
            struct apage p = at[i].pg[b];
            if (p.cell != 0 && !p.shared) {
                p.cell = cell_new(cell[p.cell]); /* its own copy */
            }
            at[c].pg[b] = p;
        }
        return 1;
    }
    case 13: { /* share with another live task: from a page in a region, a short run */
        a = pick_used(i, 0);
        n = 1u + pick(4);
        if (a + n > WN) {
            n = WN - a;
        }
        unsigned o = live_task();
        uint64_t oa = pick_va();
        if (oa + n > WN) {
            oa = WN - n;
        }
        uint32_t id = at[i].pg[a].region;
        int one = id != 0;
        int clash = 0;
        for (uint64_t b = 0; b < n; b++) {
            one &= at[i].pg[a + b].region == id;
            clash |= o < NT && at[o].pg[oa + b].region != 0;
        }
        if (o == NT) {
            return 1;
        }
        int e = mm_share(&m, t, WLO + a, n, at[o].mt, WLO + oa);
        int want = o == i || !one || clash ? MM_E_STATE : MM_OK;
        if (e != want && !(want == MM_OK && e == MM_E_NOMEM)) {
            WRONG("share(task %u 0x%llx %llu, task %u 0x%llx): %d, expected %d", i, (unsigned long long)(WLO + a),
                  (unsigned long long)n, o, (unsigned long long)(WLO + oa), e, want);
        }
        cov.shares += e == MM_OK;
        if (e == MM_OK) {
            uint32_t after = a + n < WN ? at[i].pg[a + n].region : 0;
            uint32_t ti = ++next_region;
            uint32_t oi = ++next_region;
            for (uint64_t b = 0; b < n; b++) {
                struct apage *p = &at[i].pg[a + b];
                if (p->cell == 0) {
                    p->cell = cell_new(0);
                }
                p->region = ti;
                p->shared = 1;
                at[o].pg[oa + b] = (struct apage){.region = oi, .perms = p->perms, .shared = 1, .cell = p->cell};
            }
            if (after == id && id != 0) {
                renumber_after(i, a + n, id);
            }
        }
        return 1;
    }
    case 14: /* exit, now and then */
        if (pick(4) != 0) {
            return 1;
        }
        if (joist_task_destroy(&k, root, TSLOT(i)) != JOIST_OK ||
            joist_cap_delete(&k, root, TSLOT(i)) != JOIST_OK) {
            WRONG("task %u could not be destroyed", i);
        }
        mm_task_gone(&m, t);
        at[i].live = 0;
        cov.exits++;
        return 1;
    default: /* read back a page */
        return do_read(i, pick_used(i, 0));
    }
}

/* mm's records and the kernel's mappings agree, task by task. */
static int covered(void)
{
    for (unsigned i = 0; i < NT; i++) {
        if (!at[i].live) {
            continue;
        }
        const struct mm_task *tk = &m.task[at[i].mt];
        uint32_t j = 0;
        for (uint64_t va = WLO; va < WLO + WN; va++) {
            uint64_t e = pte(at[i].s, va);
            int known = j < tk->nmap && tk->map[j].va == va;
            if ((e != 0) != known) {
                WRONG("task %u page 0x%llx: the kernel %s it, mm %s", i, (unsigned long long)va,
                      e != 0 ? "maps" : "doesn't map", known ? "records it" : "doesn't");
            }
            if (known) {
                const struct mm_page *g = &m.pg[tk->map[j].page];
                if (JOIST_PTE_OFF(e) != g->off || k.cs[root].slot[POOL].obj != JOIST_PTE_FRAME(e) + 1u) {
                    WRONG("task %u page 0x%llx: the kernel's page isn't mm's", i, (unsigned long long)va);
                }
                j++;
            }
        }
        if (j != tk->nmap) {
            WRONG("task %u: mm records pages outside the window", i);
        }
    }
    return 1;
}

static int run(uint64_t seed, unsigned steps)
{
    rng = seed * 0x9e3779b97f4a7c15ull | 1u;
    if (!setup()) {
        (void)snprintf(why, sizeof why, "seed %llu: the world could not be set up", (unsigned long long)seed);
        return 0;
    }
    char msg[200];
    for (unsigned s = 0; s < steps; s++) {
        if (!step() || !covered()) {
            return 0;
        }
        if (mm_check(&m, msg, sizeof msg) != 0) {
            (void)snprintf(why, sizeof why, "%s", msg);
            return 0;
        }
        if (joist_check_invariants(&k, msg, sizeof msg) != 0) {
            (void)snprintf(why, sizeof why, "kernel: %s", msg);
            return 0;
        }
    }
    return 1;
}

/* JOIST_MM_SEEDS seeds (default 48: the exit run's M sets more), or the one
 * in JOIST_MM_SEED, to replay a failure. */
JT_TEST(mm_refines_the_abstract_machine)
{
    const char *one = getenv("JOIST_MM_SEED");
    const char *v = getenv("JOIST_MM_SEEDS");
    uint64_t first = one != NULL && *one != '\0' ? strtoull(one, NULL, 0) : 1;
    uint64_t last = one != NULL && *one != '\0' ? first
                    : v != NULL && *v != '\0'   ? strtoull(v, NULL, 0)
                                                 : 48;
    for (uint64_t seed = first; seed <= last; seed++) {
        if (!run(seed, 600)) {
            fprintf(stderr, "  mm refinement: seed %llu: %s\n", (unsigned long long)seed, why);
            JT_CHECK(0);
            return;
        }
    }
    if (one != NULL && *one != '\0') {
        return; /* one seed: the coverage floors are for many */
    }
    fprintf(stderr,
            "  coverage mm: %lu reads of written pages, %lu writes, %lu COW copies, %lu clones, "
            "%lu shares, %lu unmaps, %lu protects, %lu exits, %lu faults refused\n",
            cov.reads_written, cov.writes, cov.cow_copies, cov.clones, cov.shares, cov.unmaps,
            cov.protects, cov.exits, cov.refused);
    /* Floors: each path is taken often enough to mean something. */
    JT_CHECK(cov.reads_written > 2000 && cov.writes > 2000 && cov.cow_copies > 200);
    JT_CHECK(cov.clones > 100 && cov.shares > 100 && cov.unmaps > 200 && cov.protects > 100);
    JT_CHECK(cov.exits > 50 && cov.refused > 100);
}
