/* loader.h - what a root task needs to start other tasks from boot modules
 * (Phase 3 steps 4 and 5): a page allocator over the memory pool the kernel
 * hands it, and a loader that turns a module into a task. Shared by the root
 * tasks in user/ (root, schedroot). Header only; each includes it once.
 *
 * The including program defines nothing first; it calls ld_init() with the
 * values its _start received, then ld_load(). */
#ifndef JOIST_USER_LOADER_H
#define JOIST_USER_LOADER_H

#include "elf.h"
#include "joist.h"

#define MOD_VA      0x10000000ull /* modules, mapped read-only, 2 MiB apart */
#define SCRATCH_VA  0x20000000ull /* one page being filled */
#define CHILD_LO    0x400000ull
#define USER_TOP    0x0000800000000000ull
#define STACK_PAGES 4u
#define DONATION    64u

static uint64_t next_slot; /* slot_new() hands them out */
static uint64_t pool_cur, pool_left, pool_next, pool_end;
static uint64_t ld_mod_first;
static uint8_t ld_mapped[8]; /* modules already mapped here */

static __attribute__((noreturn)) void die(const char *why)
{
    j_puts("root: ");
    j_puts(why);
    j_puts("\n");
    j_exit(1);
}

static void must(uint64_t e, const char *what)
{
    if (e != J_OK) {
        j_puts("root: ");
        j_puts(what);
        j_puts(" failed with ");
        j_putu(e);
        j_puts("\n");
        j_exit(1);
    }
}

/* A free slot of the root's capability table. The first leaf's (slots up
 * to 127) come first; the second leaf holds the root's own image, so after
 * that the table grows a leaf at a time from the third (slot 256) on. */
#define LD_LEAF 128u
static uint64_t slot_new(void)
{
    if (next_slot == LD_LEAF) {
        next_slot = 2u * LD_LEAF;
    }
    if (next_slot >= 2u * LD_LEAF && next_slot % LD_LEAF == 0) {
        must(j_cspace_grow(next_slot / LD_LEAF), "cspace_grow");
    }
    return next_slot++;
}

/* Pages handed out, in order, and pages given back (ld_mark, ld_release):
 * a root task that makes and destroys tasks over and over reuses their
 * pages rather than splitting the pool further each time (the memory-growth
 * test needs its own use to stay level). */
#define LD_LOG 2048u
static uint64_t ld_log[LD_LOG];
static uint32_t ld_nlog;
static uint64_t ld_free[LD_LOG];
static uint32_t ld_nfree;

static uint64_t page_split(void);

/* One page of memory, as a frame of its own: one given back, or split off
 * the pool. */
static uint64_t page_alloc(void)
{
    uint64_t p = ld_nfree != 0 ? ld_free[--ld_nfree] : page_split();
    if (ld_nlog == LD_LOG) {
        die("more pages handed out than the loader keeps track of");
    }
    ld_log[ld_nlog++] = p;
    return p;
}

/* Where the pages handed out so far end. */
static __attribute__((unused)) uint32_t ld_mark(void)
{
    return ld_nlog;
}

/* Every page handed out since `mark` is free again. The caller has made
 * sure none is mapped any more (the tasks they went to are destroyed, and
 * its own mappings undone). */
static __attribute__((unused)) void ld_release(uint32_t mark)
{
    while (ld_nlog > mark) {
        ld_free[ld_nfree++] = ld_log[--ld_nlog];
    }
}

static uint64_t page_split(void)
{
    while (pool_left == 0) {
        if (pool_next == pool_end) {
            die("out of memory");
        }
        pool_cur = pool_next++;
        pool_left = j_frame_pages(pool_cur);
    }
    if (pool_left == 1) {
        pool_left = 0;
        return pool_cur;
    }
    uint64_t rest = slot_new();
    must(j_frame_split(pool_cur, 1, rest), "frame_split");
    uint64_t page = pool_cur;
    pool_cur = rest;
    pool_left--;
    return page;
}

/* A fresh page for the task in tslot at va: zeroed, with `n` bytes from
 * `from` copied to offset `off`, then mapped there with perms. */
static void give_page(uint64_t tslot, uint64_t va, unsigned perms, const uint8_t *from,
                      uint64_t off, uint64_t n)
{
    uint64_t f = page_alloc();
    uint8_t *p = (uint8_t *)(uintptr_t)SCRATCH_VA;
    must(j_map(f, J_SLOT_SELF, SCRATCH_VA / J_PAGE, J_MAP_W), "map (scratch)");
    for (uint64_t i = 0; i < J_PAGE; i++) {
        p[i] = 0; /* pool memory is whatever boot left there */
    }
    for (uint64_t i = 0; i < n; i++) {
        p[off + i] = from[i];
    }
    must(j_unmap(J_SLOT_SELF, SCRATCH_VA / J_PAGE), "unmap (scratch)");
    must(j_map(f, tslot, va / J_PAGE, perms), "map (task)");
}

/* Load module `index` as a new task in slot tslot; its entry point. */
static uint64_t ld_load(uint64_t index, uint64_t tslot)
{
    uint64_t mslot = ld_mod_first + index;
    uint64_t pages = j_frame_pages(mslot);
    if (pages == 0 || pages > J_MAP_SPAN) {
        die("a module isn't a frame of at most 512 pages");
    }
    const uint8_t *img = (const uint8_t *)(uintptr_t)(MOD_VA + index * 0x200000ull);
    if (index >= sizeof ld_mapped) {
        die("too many modules");
    }
    if (!ld_mapped[index]) { /* one module may be loaded as several tasks */
        must(j_map(mslot, J_SLOT_SELF, (uint64_t)(uintptr_t)img / J_PAGE, 0), "map (module)");
        ld_mapped[index] = 1;
    }
    struct elf_image im;
    if (elf_validate(img, pages * J_PAGE, CHILD_LO, USER_TOP, &im) != KERR_OK) {
        die("a module was refused by the ELF validator");
    }
    must(j_task_create(DONATION, tslot), "task_create");
    for (uint32_t i = 0; i < im.nsegments; i++) {
        const struct elf_segment *s = &im.seg[i];
        unsigned perms = ((s->flags & ELF_SEG_W) ? J_MAP_W : 0u) | ((s->flags & ELF_SEG_X) ? J_MAP_X : 0u);
        uint64_t first = s->vaddr & ~(uint64_t)(J_PAGE - 1u);
        for (uint64_t page = first; page < s->vaddr + s->memsz; page += J_PAGE) {
            /* The bytes of [vaddr, vaddr + filesz) that fall in this page. */
            uint64_t lo = page > s->vaddr ? page : s->vaddr;
            uint64_t hi = page + J_PAGE < s->vaddr + s->filesz ? page + J_PAGE : s->vaddr + s->filesz;
            uint64_t n = hi > lo ? hi - lo : 0;
            give_page(tslot, page, perms, img + s->offset + (lo - s->vaddr), lo - page, n);
        }
    }
    for (unsigned i = 0; i < STACK_PAGES; i++) {
        give_page(tslot, CHILD_LO - (uint64_t)(i + 1u) * J_PAGE, J_MAP_W, img, 0, 0);
    }
    return im.entry;
}

/* A second stack in the task in tslot, for a second thread: STACK_PAGES
 * pages below `top`. */
static __attribute__((unused)) void ld_stack(uint64_t tslot, uint64_t top)
{
    for (unsigned i = 0; i < STACK_PAGES; i++) {
        give_page(tslot, top - (uint64_t)(i + 1u) * J_PAGE, J_MAP_W, (const uint8_t *)"", 0, 0);
    }
}

/* Start from what the kernel passed the root task (kernel/arch/x86_64/run.h). */
static void ld_init(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t first_free)
{
    next_slot = first_free;
    pool_next = pool_first;
    pool_end = pool_first + pool_n;
    ld_mod_first = mod_first;
}

#endif /* JOIST_USER_LOADER_H */
