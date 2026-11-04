/* SPDX-License-Identifier: GPL-3.0-only */
/* mmlib.c - the memory manager's logic (decision 0023). See mmlib.h.
 *
 * The pool of pages, the tasks mm serves, their regions of anonymous
 * memory, unmapping and protection, demand-zero faults (part A of Phase
 * 4's item 6); clone with copy on write, and sharing between tasks (part
 * B). A page's references are its mappings: more than one in private
 * regions is a COW share, read-only in all of them until written; any
 * number in shared regions is a true share, never COW. */
#include "mmlib.h"

#if __STDC_HOSTED__
#include <stdio.h>
#include <string.h>
#else /* the server: the kernel's string functions are linked in */
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
#endif

#define PAGE_BYTES 4096u

/* ---- the pool ---- */

static uint8_t *bytes(const struct mm *m, uint32_t p)
{
    const struct mm_page *g = &m->pg[p];
    return m->k.page(m->k.ctx, m->frame[g->frame].slot, g->off);
}

/* ---- trees (0028) ---- */

/* Every tree's reservation it doesn't use yet: pages mm keeps free. */
static uint64_t unused_all(const struct mm *m)
{
    uint64_t u = 0;
    for (uint32_t r = 0; r < MM_MAX_TASKS; r++) {
        const struct mm_tree *g = &m->tree[r];
        u += g->live && g->reserve > g->pages ? g->reserve - g->pages : 0;
    }
    return u;
}

/* Whether tree r may take n more pages: within its ceiling, and, past its
 * reservation, from pages no tree has reserved. */
static int tree_room(const struct mm *m, uint32_t r, uint64_t n)
{
    const struct mm_tree *g = &m->tree[r];
    if (n > m->nfree || n > g->ceil || g->pages > g->ceil - n) {
        return 0;
    }
    uint64_t below = g->reserve > g->pages ? g->reserve - g->pages : 0;
    uint64_t own = n < below ? n : below; /* taken from its own reservation */
    return m->nfree - n >= unused_all(m) - own;
}

/* Tree r forgotten once it has neither tasks nor pages; with no tasks, its
 * reservation is given back at once. */
static void tree_drop(struct mm *m, uint32_t r)
{
    struct mm_tree *g = &m->tree[r];
    if (g->tasks == 0) {
        g->reserve = 0;
        if (g->pages == 0) {
            memset(g, 0, sizeof *g);
        }
    }
}

/* Page p no longer charged to its tree. */
static void page_uncharge(struct mm *m, uint32_t p)
{
    uint32_t r = m->pg[p].tree - 1u;
    m->pg[p].tree = 0;
    m->tree[r].pages--;
    tree_drop(m, r);
}

/* A free page, zeroed, with one reference, charged to tree r; -1 if none
 * is left, or r may not take it. */
static int64_t page_new(struct mm *m, uint32_t r)
{
    if (m->free == 0 || !tree_room(m, r, 1)) {
        return -1;
    }
    uint32_t p = m->free - 1u;
    m->free = m->pg[p].next;
    m->pg[p].next = 0;
    m->pg[p].refs = 1;
    m->pg[p].tree = r + 1u;
    m->tree[r].pages++;
    m->nfree--;
    memset(bytes(m, p), 0, PAGE_BYTES);
    return p;
}

static void page_unref(struct mm *m, uint32_t p)
{
    if (--m->pg[p].refs == 0) {
        page_uncharge(m, p);
        m->pg[p].next = m->free;
        m->free = p + 1u;
        m->nfree++;
    }
}

/* ---- tasks and their records ---- */

static int task_ok(const struct mm *m, uint32_t t)
{
    return t < MM_MAX_TASKS && m->task[t].live;
}

/* The region of task t holding page va, or -1. */
static int region_of(const struct mm_task *tk, uint64_t va)
{
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        const struct mm_region *r = &tk->reg[i];
        if (r->n != 0 && va >= r->va && va - r->va < r->n) {
            return (int)i;
        }
    }
    return -1;
}

/* The first mapping at or after va (an index into tk->map; nmap if none). */
static uint32_t map_from(const struct mm_task *tk, uint64_t va)
{
    uint32_t lo = 0;
    uint32_t hi = tk->nmap;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (tk->map[mid].va < va) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* The mapping of page va, or -1. */
static int64_t map_at(const struct mm_task *tk, uint64_t va)
{
    uint32_t i = map_from(tk, va);
    return i < tk->nmap && tk->map[i].va == va ? (int64_t)i : -1;
}

/* Record that task tk maps page p at va (not already mapped there). */
static int map_add(struct mm_task *tk, uint64_t va, uint32_t p)
{
    if (tk->nmap + tk->rpages >= MM_TASK_PAGES) {
        return MM_E_NOMEM;
    }
    uint32_t i = map_from(tk, va);
    memmove(&tk->map[i + 1u], &tk->map[i], (tk->nmap - i) * sizeof tk->map[0]);
    tk->map[i] = (struct mm_map){.va = va, .page = p};
    tk->nmap++;
    return MM_OK;
}

static int bad_range(uint64_t va, uint64_t n)
{
    return n == 0 || va > UINT64_MAX - n;
}

static int bad_perms(unsigned perms)
{
    return (perms & ~(MM_W | MM_X | MM_NONE)) != 0 || perms == (MM_W | MM_X) ||
           ((perms & MM_NONE) != 0 && perms != MM_NONE);
}

/* ---- region capabilities' records (0030) ---- */

/* The region capability task t names by tslot, or -1. */
static int kreg_of(const struct mm *m, uint32_t t, uint32_t tslot)
{
    for (uint32_t i = 0; i < MM_KREGIONS; i++) {
        const struct mm_kregion *r = &m->kreg[i];
        if (r->live && !r->lost && r->task == t && r->tslot == tslot) {
            return (int)i;
        }
    }
    return -1;
}

static uint64_t kreg_end(const struct mm_kregion *r)
{
    return r->lo + (uint64_t)r->tables * MM_SPAN;
}

/* Whether [va, va + n) meets one of task t's region capabilities. */
static int meets_kreg(const struct mm *m, uint32_t t, uint64_t va, uint64_t n)
{
    for (uint32_t i = 0; i < MM_KREGIONS; i++) {
        const struct mm_kregion *r = &m->kreg[i];
        if (r->live && !r->lost && r->task == t && va < kreg_end(r) && r->lo < va + n) {
            return 1;
        }
    }
    return 0;
}

static uint64_t kreg_pages(const struct mm_kregion *r)
{
    uint64_t n = 0;
    for (uint32_t x = 0; x < r->next; x++) {
        n += r->ext[x].n;
    }
    return n;
}

/* n free pages in a row of one frame, taken for region capability i and
 * charged to tree r (off the free list, which keeps its order): the
 * first's index, or -1. The caller checked r may take them. */
static int64_t run_take(struct mm *m, uint32_t n, uint32_t i, uint32_t r)
{
    uint32_t len = 0;
    for (uint32_t p = 0; p < m->npages; p++) {
        if (m->pg[p].refs != 0 || m->pg[p].kreg != 0) {
            len = 0;
            continue;
        }
        len = len != 0 && m->pg[p].frame == m->pg[p - 1u].frame ? len + 1u : 1u;
        if (len < n) {
            continue;
        }
        uint32_t first = p + 1u - n;
        for (uint32_t q = first; q <= p; q++) {
            m->pg[q].kreg = i + 1u;
            m->pg[q].tree = r + 1u;
        }
        m->tree[r].pages += n;
        for (uint32_t *at = &m->free; *at != 0;) {
            struct mm_page *g = &m->pg[*at - 1u];
            if (g->kreg != 0) {
                *at = g->next;
                g->next = 0;
            } else {
                at = &g->next;
            }
        }
        m->nfree -= n;
        return first;
    }
    return -1;
}

/* Pages [first, first + n) back in the pool, the last first (so they come
 * back out in order: see mm_unmap). */
static void run_give(struct mm *m, uint32_t first, uint32_t n)
{
    for (uint32_t k = n; k-- > 0;) {
        struct mm_page *g = &m->pg[first + k];
        if (g->tree != 0) {
            page_uncharge(m, first + k);
        }
        g->kreg = 0;
        g->next = m->free;
        m->free = first + k + 1u;
        m->nfree++;
    }
}

/* Region capability i's record gone, its pages back in the pool: the kernel
 * maps none of them now (its range unmapped, or its task gone). */
static void kreg_release(struct mm *m, uint32_t i)
{
    struct mm_kregion *r = &m->kreg[i];
    for (uint32_t x = 0; x < r->next; x++) {
        run_give(m, r->ext[x].page, r->ext[x].n);
    }
    memset(r, 0, sizeof *r);
}

/* Pages being mapped into task t with one kernel call: consecutive pages
 * of its space from va, mm's pages from p (so consecutive offsets in one
 * frame), the same permissions, within one table's span (what one call may
 * map). Each is recorded before it joins the run, as if mapped. */
struct run {
    uint32_t t;
    uint32_t p;
    uint64_t va, n;
    unsigned perms;
};

/* The run's records undone: its pages were never mapped. */
static void run_undo(struct mm *m, const struct run *r)
{
    struct mm_task *tk = &m->task[r->t];
    uint32_t i = map_from(tk, r->va);
    for (uint64_t k = r->n; k-- > 0;) {
        page_unref(m, tk->map[i + k].page); /* the last first: see mm_unmap */
    }
    memmove(&tk->map[i], &tk->map[i + r->n], (tk->nmap - i - r->n) * sizeof tk->map[0]);
    tk->nmap -= (uint32_t)r->n;
}

/* Map the run, if it has pages, and empty it. The kernel maps all of it or
 * none: if none, its records go too. */
static int run_flush(struct mm *m, struct run *r)
{
    if (r->n == 0) {
        return MM_OK;
    }
    const struct mm_page *g = &m->pg[r->p];
    int e = m->k.map(m->k.ctx, m->frame[g->frame].slot, g->off, r->n, m->task[r->t].slot, r->va,
                     r->perms) == 0
                ? MM_OK
                : MM_E_KERNEL;
    if (e != MM_OK) {
        run_undo(m, r);
    }
    r->n = 0;
    return e;
}

/* Page p, just recorded at va in task t, mapped with perms: added to the
 * run if it continues it, else the run so far is mapped and a new one
 * begins with it. If that mapping fails, p's record goes too. */
static int run_add(struct mm *m, struct run *r, uint32_t t, uint64_t va, uint32_t p, unsigned perms)
{
    if (r->n != 0 && t == r->t && va == r->va + r->n && va / MM_SPAN == r->va / MM_SPAN &&
        p == r->p + r->n && m->pg[p].frame == m->pg[r->p].frame && perms == r->perms) {
        r->n++;
        return MM_OK;
    }
    int e = run_flush(m, r);
    *r = (struct run){.t = t, .p = p, .va = va, .n = 1, .perms = perms};
    if (e != MM_OK) {
        run_undo(m, r);
        r->n = 0;
    }
    return e;
}

/* ---- set-up ---- */

void mm_init(struct mm *m, const struct mm_kops *k)
{
    memset(m, 0, sizeof *m);
    m->k = *k;
}

int mm_add_frame(struct mm *m, uint32_t frame_slot, uint64_t npages)
{
    if (m->nframes == MM_FRAMES || npages == 0 || npages > MM_PAGES - m->npages) {
        return MM_E_NOMEM;
    }
    uint32_t f = m->nframes++;
    m->frame[f] = (struct mm_frame){.slot = frame_slot, .npages = npages};
    for (uint64_t i = npages; i-- > 0;) {
        uint32_t p = m->npages + (uint32_t)i;
        m->pg[p] = (struct mm_page){.frame = f, .off = (uint32_t)i, .refs = 0, .next = m->free};
        m->free = p + 1u;
        m->nfree++;
    }
    m->npages += (uint32_t)npages;
    return MM_OK;
}

/* A free task record for slot, in tree r (whose count it joins). */
static int task_in(struct mm *m, uint32_t slot, uint32_t r, uint32_t *t)
{
    for (uint32_t i = 0; i < MM_MAX_TASKS; i++) {
        if (!m->task[i].live) {
            memset(&m->task[i], 0, sizeof m->task[i]);
            m->task[i].live = 1;
            m->task[i].slot = slot;
            m->task[i].tree = r;
            m->tree[r].tasks++;
            *t = i;
            return MM_OK;
        }
    }
    return MM_E_NOMEM;
}

int mm_task_add(struct mm *m, uint32_t slot, uint32_t *t)
{
    uint32_t r = 0;
    while (r < MM_MAX_TASKS && m->tree[r].live) {
        r++;
    }
    /* Admission (0028): its reservation must fit beside the others'. */
    if (r == MM_MAX_TASKS || m->nfree < unused_all(m) + MM_TREE_RESERVE) {
        return MM_E_NOMEM;
    }
    m->tree[r] = (struct mm_tree){.live = 1,
                                  .reserve = MM_TREE_RESERVE,
                                  .ceil = (m->npages - m->lost) / 2u,
                                  .ceil_tasks = MM_MAX_TASKS / 2u};
    int e = task_in(m, slot, r, t);
    if (e != MM_OK) {
        memset(&m->tree[r], 0, sizeof m->tree[r]);
    }
    return e;
}

int mm_task_add_child(struct mm *m, uint32_t slot, uint32_t parent, uint32_t *t)
{
    if (!task_ok(m, parent)) {
        return MM_E_STATE;
    }
    uint32_t r = m->task[parent].tree;
    if (m->tree[r].tasks >= m->tree[r].ceil_tasks) {
        return MM_E_NOMEM; /* fork's EAGAIN */
    }
    return task_in(m, slot, r, t);
}

int mm_set_limit(struct mm *m, uint32_t t, uint64_t reserve, uint64_t ceil, uint32_t ceil_tasks)
{
    if (!task_ok(m, t)) {
        return MM_E_STATE;
    }
    struct mm_tree *g = &m->tree[m->task[t].tree];
    uint64_t was = g->reserve > g->pages ? g->reserve - g->pages : 0;
    uint64_t now = reserve > g->pages ? reserve - g->pages : 0;
    if (m->nfree < unused_all(m) - was + now) {
        return MM_E_NOMEM;
    }
    g->reserve = reserve;
    g->ceil = ceil;
    g->ceil_tasks = ceil_tasks;
    return MM_OK;
}

void mm_task_gone(struct mm *m, uint32_t t)
{
    if (!task_ok(m, t)) {
        return;
    }
    struct mm_task *tk = &m->task[t];
    /* Its space is gone with its tables: only mm's records are left. */
    for (uint32_t i = tk->nmap; i-- > 0;) {
        page_unref(m, tk->map[i].page); /* the last first: see mm_unmap */
    }
    /* Its regions' extents went with its space (the kernel releases them):
     * mm's capabilities, dead, go, and the pages come back. */
    for (uint32_t i = 0; i < MM_KREGIONS; i++) {
        const struct mm_kregion *r = &m->kreg[i];
        if (r->live && !r->lost && r->task == t) {
            (void)m->k.del(m->k.ctx, m->kreg_slot + i);
            kreg_release(m, i);
        }
    }
    uint32_t r = tk->tree;
    memset(tk, 0, sizeof *tk);
    m->tree[r].tasks--;
    tree_drop(m, r);
}

/* ---- requests ---- */

int mm_map_anon(struct mm *m, uint32_t t, uint64_t va, uint64_t n, unsigned perms)
{
    if (!task_ok(m, t)) {
        return MM_E_STATE;
    }
    if (bad_range(va, n) || bad_perms(perms)) {
        return MM_E_RANGE;
    }
    struct mm_task *tk = &m->task[t];
    int free_at = -1;
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        const struct mm_region *r = &tk->reg[i];
        if (r->n == 0) {
            free_at = free_at < 0 ? (int)i : free_at;
        } else if (va < r->va + r->n && r->va < va + n) {
            return MM_E_STATE; /* overlaps a region */
        }
    }
    if (meets_kreg(m, t, va, n)) {
        return MM_E_STATE; /* a region capability's: the task maps there itself */
    }
    if (free_at < 0) {
        return MM_E_NOMEM;
    }
    tk->reg[free_at] = (struct mm_region){.va = va, .n = n, .perms = perms};
    return MM_OK;
}

/* Cut [va, va + n) out of task tk's regions: a region inside it goes, one
 * across an edge shrinks, one across both splits in two. Refused, changing
 * nothing, if a split needs a free entry and there is none. */
static int regions_cut(struct mm_task *tk, uint64_t va, uint64_t n)
{
    unsigned splits = 0;
    unsigned free_n = 0;
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        const struct mm_region *r = &tk->reg[i];
        free_n += r->n == 0;
        splits += r->n != 0 && r->va < va && va + n < r->va + r->n;
    }
    if (splits > free_n) {
        return MM_E_NOMEM;
    }
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        struct mm_region *r = &tk->reg[i];
        if (r->n == 0 || va + n <= r->va || r->va + r->n <= va) {
            continue;
        }
        uint64_t end = r->va + r->n;
        if (va <= r->va && end <= va + n) {
            r->n = 0; /* all of it */
        } else if (r->va < va && va + n < end) {
            for (unsigned j = 0; j < MM_REGIONS; j++) {
                if (tk->reg[j].n == 0) {
                    tk->reg[j] = (struct mm_region){
                        .va = va + n, .n = end - (va + n), .perms = r->perms, .shared = r->shared};
                    break;
                }
            }
            r->n = va - r->va;
        } else if (r->va < va) {
            r->n = va - r->va; /* its tail */
        } else {
            r->n = end - (va + n); /* its head */
            r->va = va + n;
        }
    }
    return MM_OK;
}

static int kernel_unmap(struct mm *m, struct mm_task *tk, uint64_t va, uint64_t n, uint32_t *last);
static int unmap_pages(struct mm *m, struct mm_task *tk, uint64_t va, uint64_t n);

int mm_unmap(struct mm *m, uint32_t t, uint64_t va, uint64_t n)
{
    if (!task_ok(m, t)) {
        return MM_E_STATE;
    }
    if (bad_range(va, n)) {
        return MM_E_RANGE;
    }
    struct mm_task *tk = &m->task[t];
    int e = regions_cut(tk, va, n);
    if (e != MM_OK) {
        return e;
    }
    return unmap_pages(m, tk, va, n);
}

/* The kernel's mappings of task tk's pages in [va, va + n) removed, a
 * table's span at a time where mm has pages; mm's records stay. The index
 * past the range's records in *last, if last isn't NULL. */
static int kernel_unmap(struct mm *m, struct mm_task *tk, uint64_t va, uint64_t n, uint32_t *last)
{
    uint32_t i = map_from(tk, va);
    while (i < tk->nmap && tk->map[i].va < va + n) {
        uint64_t lo = tk->map[i].va;
        uint64_t chunk_end = (lo / MM_SPAN + 1u) * MM_SPAN;
        uint64_t hi = chunk_end < va + n ? chunk_end : va + n;
        if (m->k.unmap(m->k.ctx, tk->slot, lo, hi - lo) != 0) {
            return MM_E_KERNEL;
        }
        while (i < tk->nmap && tk->map[i].va < hi) {
            i++;
        }
    }
    if (last != NULL) {
        *last = i;
    }
    return MM_OK;
}

/* The pages task tk has in [va, va + n) unmapped and given back: the
 * kernel first, then the pages, so none is free while still mapped. */
static int unmap_pages(struct mm *m, struct mm_task *tk, uint64_t va, uint64_t n)
{
    uint32_t first = map_from(tk, va);
    uint32_t last;
    if (kernel_unmap(m, tk, va, n, &last) != MM_OK) {
        return MM_E_KERNEL;
    }
    /* The last first, so the free list gives them back in order: pages
     * taken together again are a run in their frame (one map call). */
    for (uint32_t i = last; i-- > first;) {
        page_unref(m, tk->map[i].page); /* unmapped above */
    }
    memmove(&tk->map[first], &tk->map[last], (tk->nmap - last) * sizeof tk->map[0]);
    tk->nmap -= last - first;
    return MM_OK;
}

/* The permissions page p takes in a region with perms: in a private
 * region, no write while COW-shared (it is copied on the first write). */
static unsigned page_perms(const struct mm *m, uint32_t p, unsigned perms, int shared)
{
    return !shared && m->pg[p].refs > 1u ? perms & ~MM_W : perms;
}

int mm_protect(struct mm *m, uint32_t t, uint64_t va, uint64_t n, unsigned perms)
{
    if (!task_ok(m, t)) {
        return MM_E_STATE;
    }
    if (bad_range(va, n) || bad_perms(perms)) {
        return MM_E_RANGE;
    }
    struct mm_task *tk = &m->task[t];
    /* Every page of the range must be in a region (as mprotect's ENOMEM). */
    /* ...and all of them shared, or none (the range becomes one region). */
    uint64_t covered = 0;
    int shared = -1;
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        const struct mm_region *r = &tk->reg[i];
        if (r->n != 0 && va < r->va + r->n && r->va < va + n) {
            uint64_t lo = r->va > va ? r->va : va;
            uint64_t hi = r->va + r->n < va + n ? r->va + r->n : va + n;
            covered += hi - lo;
            if (shared >= 0 && shared != r->shared) {
                return MM_E_STATE;
            }
            shared = r->shared;
        }
    }
    if (covered != n) {
        return MM_E_STATE;
    }
    /* Cut the range out and put it back whole with the new permissions.
     * That takes an entry for each region split in two and one for the
     * range; the free entries and the regions wholly inside (which the cut
     * frees) are what there is. */
    unsigned need = 1;
    unsigned have = 0;
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        const struct mm_region *r = &tk->reg[i];
        have += r->n == 0 || (va <= r->va && r->va + r->n <= va + n);
        need += r->n != 0 && r->va < va && va + n < r->va + r->n;
    }
    if (need > have) {
        return MM_E_NOMEM;
    }
    (void)regions_cut(tk, va, n);
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        if (tk->reg[i].n == 0) {
            tk->reg[i] = (struct mm_region){.va = va, .n = n, .perms = perms, .shared = (uint8_t)shared};
            break;
        }
    }
    /* No access: the kernel's mappings go (x86 has no present entry that
     * refuses reads); mm keeps the pages, mapped again at their first
     * fault once the range is accessible. */
    if (perms == MM_NONE) {
        return kernel_unmap(m, tk, va, n, NULL);
    }
    /* The mapped pages: runs of the same permissions within one span. */
    uint32_t i = map_from(tk, va);
    while (i < tk->nmap && tk->map[i].va < va + n) {
        uint64_t lo = tk->map[i].va;
        unsigned pp = page_perms(m, tk->map[i].page, perms, shared);
        uint32_t j = i + 1u;
        while (j < tk->nmap && tk->map[j].va == lo + (j - i) && tk->map[j].va < va + n &&
               tk->map[j].va / MM_SPAN == lo / MM_SPAN &&
               page_perms(m, tk->map[j].page, perms, shared) == pp) {
            j++;
        }
        if (m->k.protect(m->k.ctx, tk->slot, lo, j - i, pp) != 0) {
            return MM_E_KERNEL;
        }
        i = j;
    }
    return MM_OK;
}

/* ---- faults ---- */

/* Task t's mapping mi is a COW share: give it a copy of its own, mapped in
 * the old page's place with perms, conditionally (0020: the replace fails,
 * changing nothing, unless the old page is still there). */
static int cow_break(struct mm *m, uint32_t t, uint32_t mi, unsigned perms)
{
    struct mm_task *tk = &m->task[t];
    uint32_t p = tk->map[mi].page;
    int64_t q = page_new(m, tk->tree);
    if (q < 0) {
        return MM_E_NOMEM;
    }
    memcpy(bytes(m, (uint32_t)q), bytes(m, p), PAGE_BYTES);
    const struct mm_page *gq = &m->pg[q];
    const struct mm_page *gp = &m->pg[p];
    int e = m->k.map_replace(m->k.ctx, m->frame[gq->frame].slot, gq->off, tk->slot,
                             tk->map[mi].va, perms, m->frame[gp->frame].slot, gp->off);
    if (e == MM_KE_STATE) {
        /* Not mapped (its range was of no access since): map the copy. */
        e = m->k.map(m->k.ctx, m->frame[gq->frame].slot, gq->off, 1, tk->slot, tk->map[mi].va,
                     perms);
    }
    if (e != 0) {
        page_unref(m, (uint32_t)q);
        return MM_E_KERNEL;
    }
    tk->map[mi].page = (uint32_t)q;
    page_unref(m, p);
    return MM_OK;
}

/* Map fresh zeroed pages into task t from va: one for a write, and for a
 * read up to MM_AROUND, while each is unmapped, in the region and in va's
 * span; in runs. It stops at the first page it can't take or map. 1 if the
 * faulting page is there. */
static int demand_zero(struct mm *m, uint32_t t, const struct mm_region *r, uint64_t va,
                       int write)
{
    struct mm_task *tk = &m->task[t];
    uint64_t end = r->va + r->n;
    uint64_t span_end = (va / MM_SPAN + 1u) * MM_SPAN;
    end = end < span_end ? end : span_end;
    unsigned want = write ? 1u : MM_AROUND;
    struct run run = {0};
    for (unsigned k = 0; k < want && va + k < end; k++) {
        uint64_t at = va + k;
        if (map_at(tk, at) >= 0) {
            break;
        }
        int64_t p = page_new(m, tk->tree);
        if (p < 0) {
            break;
        }
        if (map_add(tk, at, (uint32_t)p) != MM_OK) {
            page_unref(m, (uint32_t)p);
            break;
        }
        if (run_add(m, &run, t, at, (uint32_t)p, r->perms) != MM_OK) {
            return map_at(tk, va) >= 0;
        }
    }
    (void)run_flush(m, &run);
    return map_at(tk, va) >= 0;
}

int mm_fault(struct mm *m, uint32_t t, uint64_t va, unsigned access)
{
    if (!task_ok(m, t)) {
        return 0;
    }
    struct mm_task *tk = &m->task[t];
    int ri = region_of(tk, va);
    if (ri < 0) {
        return 0; /* outside every region */
    }
    const struct mm_region *r = &tk->reg[ri];
    if (r->perms == MM_NONE) {
        return 0; /* a guard: no access */
    }
    int write = (access & MM_ACCESS_WRITE) != 0;
    if ((write && (r->perms & MM_W) == 0) || ((access & MM_ACCESS_EXEC) && (r->perms & MM_X) == 0)) {
        return 0; /* not allowed by the region */
    }
    int64_t mi = map_at(tk, va);
    if (mi < 0) {
        return demand_zero(m, t, r, va, write);
    }
    uint32_t p = tk->map[mi].page;
    if ((access & MM_ACCESS_PRESENT) == 0) {
        /* mm has the page but the kernel doesn't map it: another thread's
         * fault got there first (the map finds it there), or its range was
         * of no access since. A COW page goes back read-only; a write then
         * faults again, and copies. */
        int e = m->k.map(m->k.ctx, m->frame[m->pg[p].frame].slot, m->pg[p].off, 1, tk->slot, va,
                         page_perms(m, p, r->perms, r->shared));
        return e == 0 || e == MM_KE_OCCUPIED;
    }
    if (!write) {
        return 1; /* another thread's fault got there first */
    }
    /* A write to a page that is there but not writable: in a private
     * region, a COW share (copy it, unless this is its last reference). */
    if (!r->shared && m->pg[p].refs > 1u) {
        return cow_break(m, t, (uint32_t)mi, r->perms) == MM_OK;
    }
    return m->k.protect(m->k.ctx, tk->slot, va, 1, r->perms) == 0;
}

/* ---- region capabilities (0030) ---- */

/* The kernel's refusal as mm's. */
static int kerr(int e)
{
    if (e == MM_KE_NOMEM) {
        return MM_E_NOMEM;
    }
    if (e == MM_KE_STATE || e == MM_KE_OCCUPIED) {
        return MM_E_STATE; /* a table or region there; the task's slot taken */
    }
    return e == MM_KE_RANGE ? MM_E_RANGE : MM_E_KERNEL;
}

int mm_region_new(struct mm *m, uint32_t t, uint64_t lo, uint32_t tables, unsigned perms,
                  uint32_t tslot)
{
    if (!task_ok(m, t)) {
        return MM_E_STATE;
    }
    if (tables == 0 || tables > MM_REGION_TABLES || lo % MM_SPAN != 0 ||
        lo > UINT64_MAX - (uint64_t)tables * MM_SPAN || (perms & ~(MM_W | MM_X)) != 0) {
        return MM_E_RANGE;
    }
    const struct mm_task *tk = &m->task[t];
    uint64_t n = (uint64_t)tables * MM_SPAN;
    if (kreg_of(m, t, tslot) >= 0 || meets_kreg(m, t, lo, n)) {
        return MM_E_STATE; /* the slot names one already, or the range meets one */
    }
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        const struct mm_region *r = &tk->reg[i];
        if (r->n != 0 && lo < r->va + r->n && r->va < lo + n) {
            return MM_E_STATE; /* mm's own records: anonymous memory there */
        }
    }
    uint32_t i = 0;
    while (i < MM_KREGIONS && m->kreg[i].live) {
        i++;
    }
    if (i == MM_KREGIONS) {
        return MM_E_NOMEM;
    }
    uint32_t slot = m->kreg_slot + i;
    int e = m->k.region_create(m->k.ctx, tk->slot, lo, tables, perms, slot);
    if (e != 0) {
        return kerr(e);
    }
    e = m->k.give(m->k.ctx, tk->slot, slot, tslot, MM_R_MAP);
    if (e != 0) {
        /* Its only capability: deleting it ends it (nothing to unmap). */
        (void)m->k.del(m->k.ctx, slot);
        (void)m->k.finish(m->k.ctx);
        return kerr(e);
    }
    m->kreg[i] = (struct mm_kregion){
        .live = 1, .task = t, .tslot = tslot, .lo = lo, .tables = tables, .perms = perms};
    return MM_OK;
}

int mm_region_grow(struct mm *m, uint32_t t, uint32_t tslot, uint64_t n, uint32_t *ext)
{
    if (!task_ok(m, t)) {
        return MM_E_STATE;
    }
    int i = kreg_of(m, t, tslot);
    if (i < 0) {
        return MM_E_STATE;
    }
    if (n == 0 || n > MM_PAGES) {
        return MM_E_RANGE;
    }
    struct mm_kregion *r = &m->kreg[i];
    struct mm_task *tk = &m->task[t];
    if (r->next == MM_REGION_EXTENTS || n > MM_TASK_PAGES - tk->nmap - tk->rpages ||
        !tree_room(m, tk->tree, n)) {
        return MM_E_NOMEM;
    }
    int64_t p = run_take(m, (uint32_t)n, (uint32_t)i, tk->tree);
    if (p < 0) {
        return MM_E_NOMEM; /* no run that long in one frame */
    }
    for (uint32_t k = 0; k < n; k++) {
        memset(bytes(m, (uint32_t)p + k), 0, PAGE_BYTES);
    }
    const struct mm_page *g = &m->pg[p];
    uint32_t x = 0;
    int e = m->k.region_add(m->k.ctx, m->kreg_slot + (uint32_t)i, m->frame[g->frame].slot, g->off, n,
                            &x);
    if (e != 0) {
        run_give(m, (uint32_t)p, (uint32_t)n);
        return kerr(e);
    }
    r->ext[r->next++] = (struct mm_kextent){.page = (uint32_t)p, .n = (uint32_t)n};
    tk->rpages += (uint32_t)n;
    *ext = x;
    return MM_OK;
}

int mm_region_free(struct mm *m, uint32_t t, uint32_t tslot)
{
    if (!task_ok(m, t)) {
        return MM_E_STATE;
    }
    int i = kreg_of(m, t, tslot);
    if (i < 0) {
        return MM_E_STATE;
    }
    uint32_t slot = m->kreg_slot + (uint32_t)i;
    if (m->k.kill(m->k.ctx, slot) != 0) {
        return MM_E_KERNEL;
    }
    /* Its pages may be handed out again only once the kernel has unmapped
     * them: its range's sweep is on the destruction work list. */
    int done = m->k.finish(m->k.ctx);
    (void)m->k.del(m->k.ctx, slot);
    struct mm_kregion *r = &m->kreg[i];
    m->task[t].rpages -= (uint32_t)kreg_pages(r);
    if (!done) {
        /* Out of the pool for good: no tree pays for them any more. */
        for (uint32_t x = 0; x < r->next; x++) {
            for (uint32_t k = 0; k < r->ext[x].n; k++) {
                page_uncharge(m, r->ext[x].page + k);
            }
            m->lost += r->ext[x].n;
        }
        r->lost = 1;
        r->task = MM_MAX_TASKS;
        return MM_OK;
    }
    kreg_release(m, (uint32_t)i);
    return MM_OK;
}

/* Child region capability j's pages mapped as parent's i maps its own:
 * read back with map_query, a frame of i's extents at a time, and mapped in
 * runs (consecutive pages of one extent, the same permissions). */
struct krun {
    uint32_t ext;
    uint64_t page, va, n;
    unsigned perms;
};

static int krun_flush(struct mm *m, uint32_t j, struct krun *r)
{
    if (r->n == 0) {
        return MM_OK;
    }
    int e = m->k.region_map(m->k.ctx, m->kreg_slot + j, r->ext, r->page, r->n, r->va, r->perms);
    r->n = 0;
    return e == 0 ? MM_OK : MM_E_KERNEL;
}

static int kreg_copy_maps(struct mm *m, uint32_t i, uint32_t j)
{
    const struct mm_kregion *pr = &m->kreg[i];
    uint32_t pslot = m->task[pr->task].slot;
    uint64_t q[MM_QUERY_MAX];
    for (uint32_t x = 0; x < pr->next; x++) {
        uint32_t f = m->pg[pr->ext[x].page].frame;
        int seen = 0;
        for (uint32_t y = 0; y < x; y++) {
            seen |= m->pg[pr->ext[y].page].frame == f;
        }
        if (seen) {
            continue; /* that frame's pages are done */
        }
        for (uint64_t va = pr->lo; va < kreg_end(pr); va += MM_QUERY_MAX) {
            if (m->k.query(m->k.ctx, pslot, va, MM_QUERY_MAX, m->frame[f].slot, q) != 0) {
                return MM_E_KERNEL;
            }
            struct krun run = {0};
            for (uint32_t k = 0; k < MM_QUERY_MAX; k++) {
                uint64_t w = q[k];
                if ((w & MM_QUERY_PRESENT) == 0 || (w & MM_QUERY_FRAME) == 0) {
                    if (krun_flush(m, j, &run) != MM_OK) {
                        return MM_E_KERNEL;
                    }
                    continue;
                }
                uint32_t off = (uint32_t)(w >> 32);
                unsigned perms = (unsigned)(w & (MM_W | MM_X));
                uint32_t y = 0;
                uint64_t page = 0;
                for (; y < pr->next; y++) {
                    const struct mm_page *g = &m->pg[pr->ext[y].page];
                    if (g->frame == f && off >= g->off && off - g->off < pr->ext[y].n) {
                        page = off - g->off;
                        break;
                    }
                }
                if (y == pr->next) {
                    return MM_E_KERNEL; /* not its extents' (the kernel's I12 says never) */
                }
                if (run.n != 0 && run.ext == y && run.page + run.n == page &&
                    run.va + run.n == va + k && run.perms == perms) {
                    run.n++;
                    continue;
                }
                if (krun_flush(m, j, &run) != MM_OK) {
                    return MM_E_KERNEL;
                }
                run = (struct krun){.ext = y, .page = page, .va = va + k, .n = 1, .perms = perms};
            }
            if (krun_flush(m, j, &run) != MM_OK) {
                return MM_E_KERNEL;
            }
        }
    }
    return MM_OK;
}

/* The parent's region capabilities made again for the child (mm_clone):
 * the same shape and slot, extents of the same sizes holding copies, the
 * same pages mapped. A failure leaves what was made recorded as the
 * child's, for mm_task_gone. */
static int kreg_clone(struct mm *m, uint32_t parent, uint32_t child)
{
    for (uint32_t i = 0; i < MM_KREGIONS; i++) {
        const struct mm_kregion *pr = &m->kreg[i];
        if (!pr->live || pr->lost || pr->task != parent) {
            continue;
        }
        int e = mm_region_new(m, child, pr->lo, pr->tables, pr->perms, pr->tslot);
        if (e != MM_OK) {
            return e;
        }
        uint32_t j = (uint32_t)kreg_of(m, child, pr->tslot);
        for (uint32_t x = 0; x < pr->next; x++) {
            uint32_t got;
            e = mm_region_grow(m, child, pr->tslot, pr->ext[x].n, &got);
            if (e != MM_OK) {
                return e;
            }
            for (uint32_t k = 0; k < pr->ext[x].n; k++) {
                memcpy(bytes(m, m->kreg[j].ext[x].page + k), bytes(m, pr->ext[x].page + k), PAGE_BYTES);
            }
        }
        e = kreg_copy_maps(m, i, j);
        if (e != MM_OK) {
            return e;
        }
    }
    return MM_OK;
}

/* ---- clone and share (part B) ---- */

int mm_clone(struct mm *m, uint32_t parent, uint32_t child)
{
    if (!task_ok(m, parent) || !task_ok(m, child) || parent == child ||
        m->task[child].tree != m->task[parent].tree) {
        return MM_E_STATE; /* the child is made in its parent's tree (mm_task_add_child) */
    }
    struct mm_task *pt = &m->task[parent];
    struct mm_task *ct = &m->task[child];
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        if (ct->reg[i].n != 0) {
            return MM_E_STATE; /* not a new task */
        }
    }
    if (ct->nmap != 0) {
        return MM_E_STATE;
    }
    memcpy(ct->reg, pt->reg, sizeof ct->reg);
    /* The parent's writable private pages, read-only first (0011: before
     * the child can see them), in runs within one region and one span. */
    uint32_t i = 0;
    while (i < pt->nmap) {
        uint64_t lo = pt->map[i].va;
        int ri = region_of(pt, lo);
        const struct mm_region *r = &pt->reg[ri];
        if (r->shared || (r->perms & MM_W) == 0 || m->pg[pt->map[i].page].refs > 1u) {
            i++; /* shared, or not writable now */
            continue;
        }
        uint32_t j = i + 1u;
        while (j < pt->nmap && pt->map[j].va == lo + (j - i) && region_of(pt, pt->map[j].va) == ri &&
               pt->map[j].va / MM_SPAN == lo / MM_SPAN && m->pg[pt->map[j].page].refs == 1u) {
            j++;
        }
        if (m->k.protect(m->k.ctx, pt->slot, lo, j - i, r->perms & ~MM_W) != 0) {
            return MM_E_KERNEL;
        }
        i = j;
    }
    /* Then every page into the child, in runs: read-only if private, as
     * it is if shared. In order, so the child's table stays sorted. */
    struct run run = {0};
    for (i = 0; i < pt->nmap; i++) {
        const struct mm_map *e = &pt->map[i];
        const struct mm_region *r = &pt->reg[region_of(pt, e->va)];
        unsigned perms = r->shared ? r->perms : r->perms & ~MM_W;
        ct->map[ct->nmap++] = *e;
        m->pg[e->page].refs++;
        if (perms == MM_NONE) {
            continue; /* kept, as in the parent, but not mapped */
        }
        if (run_add(m, &run, child, e->va, e->page, perms) != MM_OK) {
            return MM_E_KERNEL;
        }
    }
    if (run_flush(m, &run) != MM_OK) {
        return MM_E_KERNEL;
    }
    return kreg_clone(m, parent, child);
}

int mm_share(struct mm *m, uint32_t t, uint64_t va, uint64_t n, uint32_t o, uint64_t ova)
{
    if (!task_ok(m, t) || !task_ok(m, o) || t == o) {
        return MM_E_STATE;
    }
    if (bad_range(va, n) || bad_range(ova, n)) {
        return MM_E_RANGE;
    }
    struct mm_task *tk = &m->task[t];
    struct mm_task *ok = &m->task[o];
    int ri = region_of(tk, va);
    if (ri < 0 || va + n > tk->reg[ri].va + tk->reg[ri].n) {
        return MM_E_STATE; /* not within one of t's regions */
    }
    const struct mm_region r = tk->reg[ri];
    if (r.perms == MM_NONE) {
        return MM_E_STATE; /* no pages to share */
    }
    unsigned ofree = 0;
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        const struct mm_region *q = &ok->reg[i];
        ofree += q->n == 0;
        if (q->n != 0 && ova < q->va + q->n && q->va < ova + n) {
            return MM_E_STATE; /* o has a region there */
        }
    }
    if (meets_kreg(m, o, ova, n)) {
        return MM_E_STATE; /* or a region capability */
    }
    /* Room, all of it checked before anything changes: o's table takes n
     * pages, t's the pages not there yet; the pool, those and the COW
     * pages to copy; t's regions, the cut and the new one; o's, one. */
    if (ofree == 0 || n > MM_TASK_PAGES - ok->nmap - ok->rpages) {
        return MM_E_NOMEM;
    }
    uint64_t mapped = 0;
    uint64_t cow = 0;
    for (uint32_t i = map_from(tk, va); i < tk->nmap && tk->map[i].va < va + n; i++) {
        mapped++;
        cow += !r.shared && m->pg[tk->map[i].page].refs > 1u;
    }
    if (n - mapped > MM_TASK_PAGES - tk->nmap - tk->rpages || n - mapped + cow > m->nfree ||
        !tree_room(m, tk->tree, n - mapped + cow)) {
        return MM_E_NOMEM; /* the pages it makes are t's tree's (0028) */
    }
    unsigned need = 1;
    unsigned have = 0;
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        const struct mm_region *q = &tk->reg[i];
        have += q->n == 0 || (va <= q->va && q->va + q->n <= va + n);
        need += q->n != 0 && q->va < va && va + n < q->va + q->n;
    }
    if (need > have) {
        return MM_E_NOMEM;
    }
    /* The regions first: shared in both. */
    (void)regions_cut(tk, va, n);
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        if (tk->reg[i].n == 0) {
            tk->reg[i] = (struct mm_region){.va = va, .n = n, .perms = r.perms, .shared = 1};
            break;
        }
    }
    for (unsigned i = 0; i < MM_REGIONS; i++) {
        if (ok->reg[i].n == 0) {
            ok->reg[i] = (struct mm_region){.va = ova, .n = n, .perms = r.perms, .shared = 1};
            break;
        }
    }
    /* Then t's pages: made if not there (mapped in runs), its own if
     * COW-shared, writable as the region says. */
    struct run run = {0};
    for (uint64_t k = 0; k < n; k++) {
        int64_t mi = map_at(tk, va + k);
        if (mi < 0) {
            int64_t p = page_new(m, tk->tree);
            if (p < 0 || map_add(tk, va + k, (uint32_t)p) != MM_OK) {
                (void)run_flush(m, &run);
                return MM_E_NOMEM; /* (checked above: not reached) */
            }
            if (run_add(m, &run, t, va + k, (uint32_t)p, r.perms) != MM_OK) {
                return MM_E_KERNEL;
            }
        } else if (!r.shared && m->pg[tk->map[mi].page].refs > 1u) {
            if (cow_break(m, t, (uint32_t)mi, r.perms) != MM_OK) {
                (void)run_flush(m, &run);
                return MM_E_KERNEL;
            }
        } else if (m->k.protect(m->k.ctx, tk->slot, va + k, 1, r.perms) != 0) {
            (void)run_flush(m, &run);
            return MM_E_KERNEL;
        }
    }
    if (run_flush(m, &run) != MM_OK) {
        return MM_E_KERNEL;
    }
    /* Then all of them into o too, in runs. */
    for (uint64_t k = 0; k < n; k++) {
        uint32_t p = tk->map[map_at(tk, va + k)].page;
        (void)map_add(ok, ova + k, p);
        m->pg[p].refs++;
        if (run_add(m, &run, o, ova + k, p, r.perms) != MM_OK) {
            return MM_E_KERNEL;
        }
    }
    return run_flush(m, &run); /* o's last run */
}

/* ---- the self-check ---- */

#define FAULT(...)                                                                                 \
    do {                                                                                           \
        if (bad++ == 0 && msg != NULL && msglen > 0) {                                             \
            (void)snprintf(msg, msglen, __VA_ARGS__);                                              \
        }                                                                                          \
    } while (0)

#if !__STDC_HOSTED__
/* In the server: the first fault is reported without its details. */
static int snprintf(char *s, unsigned long n, const char *fmt, ...)
{
    (void)fmt;
    if (n != 0) {
        s[0] = '\0';
    }
    return 0;
}
#endif

unsigned mm_check(const struct mm *m, char *msg, unsigned msglen)
{
    unsigned bad = 0;
    static uint32_t refs[MM_PAGES];
    static uint32_t shared_refs[MM_PAGES]; /* of those, mappings in shared regions */
    for (uint32_t p = 0; p < MM_PAGES; p++) {
        refs[p] = 0;
        shared_refs[p] = 0;
    }
    for (uint32_t t = 0; t < MM_MAX_TASKS; t++) {
        const struct mm_task *tk = &m->task[t];
        if (!tk->live) {
            if (tk->nmap != 0) {
                FAULT("mm: task %u gone but maps %u pages", t, tk->nmap);
            }
            continue;
        }
        if (tk->nmap > MM_TASK_PAGES) {
            FAULT("mm: task %u maps %u pages, past the table", t, tk->nmap);
            continue;
        }
        for (unsigned i = 0; i < MM_REGIONS; i++) {
            const struct mm_region *a = &tk->reg[i];
            if (a->n == 0) {
                continue;
            }
            if (a->va > UINT64_MAX - a->n || bad_perms(a->perms)) {
                FAULT("mm: task %u region %u wraps or has bad permissions", t, i);
            }
            for (unsigned j = i + 1u; j < MM_REGIONS; j++) {
                const struct mm_region *b = &tk->reg[j];
                if (b->n != 0 && a->va < b->va + b->n && b->va < a->va + a->n) {
                    FAULT("mm: task %u regions %u and %u overlap", t, i, j);
                }
            }
        }
        for (uint32_t i = 0; i < tk->nmap; i++) {
            const struct mm_map *e = &tk->map[i];
            if (i != 0 && tk->map[i - 1u].va >= e->va) {
                FAULT("mm: task %u's mappings out of order at %u", t, i);
            }
            int ri = region_of(tk, e->va);
            if (ri < 0) {
                FAULT("mm: task %u maps page %llu outside its regions", t, (unsigned long long)e->va);
            }
            if (e->page >= m->npages) {
                FAULT("mm: task %u maps a page mm doesn't have", t);
                continue;
            }
            refs[e->page]++;
            shared_refs[e->page] += ri >= 0 && tk->reg[ri].shared;
        }
    }
    /* Region capabilities (0030): each page of an extent is that region's
     * alone, in one frame's run; a task's count is its regions' pages. */
    static uint32_t kpages[MM_PAGES];
    static uint32_t rpages[MM_MAX_TASKS];
    for (uint32_t p = 0; p < MM_PAGES; p++) {
        kpages[p] = 0;
    }
    for (uint32_t t = 0; t < MM_MAX_TASKS; t++) {
        rpages[t] = 0;
    }
    for (uint32_t i = 0; i < MM_KREGIONS; i++) {
        const struct mm_kregion *r = &m->kreg[i];
        if (!r->live) {
            if (r->lost || r->next != 0 || r->tables != 0) {
                FAULT("mm: free region record %u carries state", i);
            }
            continue;
        }
        if (r->lost ? r->task != MM_MAX_TASKS : !task_ok(m, r->task)) {
            FAULT("mm: region %u names no task it may", i);
            continue;
        }
        if (r->tables == 0 || r->tables > MM_REGION_TABLES || r->lo % MM_SPAN != 0 ||
            (r->perms & ~(MM_W | MM_X)) != 0 || r->next > MM_REGION_EXTENTS) {
            FAULT("mm: region %u has a bad shape", i);
            continue;
        }
        for (uint32_t x = 0; x < r->next; x++) {
            const struct mm_kextent *e = &r->ext[x];
            if (e->n == 0 || e->page >= m->npages || e->n > m->npages - e->page) {
                FAULT("mm: region %u extent %u is past the pool", i, x);
                continue;
            }
            for (uint32_t k = 0; k < e->n; k++) {
                const struct mm_page *g = &m->pg[e->page + k];
                kpages[e->page + k]++;
                if (g->kreg != i + 1u || g->frame != m->pg[e->page].frame) {
                    FAULT("mm: region %u extent %u's page %u isn't its, in one frame", i, x, k);
                }
            }
        }
        if (r->lost) {
            continue;
        }
        rpages[r->task] += (uint32_t)kreg_pages(r);
        const struct mm_task *tk = &m->task[r->task];
        for (unsigned a = 0; a < MM_REGIONS; a++) {
            if (tk->reg[a].n != 0 && r->lo < tk->reg[a].va + tk->reg[a].n && tk->reg[a].va < kreg_end(r)) {
                FAULT("mm: region %u meets task %u's anonymous region %u", i, r->task, a);
            }
        }
        for (uint32_t j = i + 1u; j < MM_KREGIONS; j++) {
            const struct mm_kregion *o = &m->kreg[j];
            if (o->live && !o->lost && o->task == r->task &&
                (o->tslot == r->tslot || (r->lo < kreg_end(o) && o->lo < kreg_end(r)))) {
                FAULT("mm: regions %u and %u of task %u share a slot or pages", i, j, r->task);
            }
        }
    }
    for (uint32_t t = 0; t < MM_MAX_TASKS; t++) {
        const struct mm_task *tk = &m->task[t];
        if (tk->rpages != rpages[t] || (tk->live && tk->nmap + tk->rpages > MM_TASK_PAGES)) {
            FAULT("mm: task %u counts %u region pages, has %u", t, tk->rpages, rpages[t]);
        }
    }
    uint32_t nfree = 0;
    for (uint32_t p = 0; p < m->npages; p++) {
        if ((m->pg[p].kreg != 0) != (kpages[p] != 0) || kpages[p] > 1u ||
            (m->pg[p].kreg != 0 && m->pg[p].refs != 0)) {
            FAULT("mm: page %u is in %u extents, marked %u, with %u mappings", p, kpages[p],
                  m->pg[p].kreg, m->pg[p].refs);
        }
        if (m->pg[p].refs != refs[p]) {
            FAULT("mm: page %u counts %u references, %u mappings name it", p, m->pg[p].refs, refs[p]);
        }
        if (shared_refs[p] != 0 && shared_refs[p] != refs[p]) {
            /* A page shared by COW and truly shared at once: a write would
             * copy it for one mapper and change it under another. */
            FAULT("mm: page %u is mapped in %u shared and %u private regions", p, shared_refs[p],
                  refs[p] - shared_refs[p]);
        }
        nfree += m->pg[p].refs == 0 && m->pg[p].kreg == 0;
    }
    if (nfree != m->nfree) {
        FAULT("mm: %u pages free, nfree says %u", nfree, m->nfree);
    }
    /* Trees (0028): each page in use charged to one live tree (a lost
     * region's to none), each tree's counts right, and every tree's unused
     * reservation kept free. */
    static uint64_t tpages[MM_MAX_TASKS];
    static uint32_t ttasks[MM_MAX_TASKS];
    for (uint32_t r = 0; r < MM_MAX_TASKS; r++) {
        tpages[r] = 0;
        ttasks[r] = 0;
    }
    uint64_t charged = 0;
    for (uint32_t p = 0; p < m->npages; p++) {
        const struct mm_page *g = &m->pg[p];
        int lostp = g->kreg != 0 && g->kreg <= MM_KREGIONS && m->kreg[g->kreg - 1u].lost;
        int used = (g->refs != 0 || g->kreg != 0) && !lostp;
        if ((g->tree != 0) != used || g->tree > MM_MAX_TASKS ||
            (g->tree != 0 && !m->tree[g->tree - 1u].live)) {
            FAULT("mm: page %u charged to tree %u, in use %d", p, g->tree, used);
            continue;
        }
        if (g->tree != 0) {
            tpages[g->tree - 1u]++;
            charged++;
        }
    }
    for (uint32_t t = 0; t < MM_MAX_TASKS; t++) {
        const struct mm_task *tk = &m->task[t];
        if (tk->live) {
            if (tk->tree >= MM_MAX_TASKS || !m->tree[tk->tree].live) {
                FAULT("mm: task %u in no live tree", t);
                continue;
            }
            ttasks[tk->tree]++;
        }
    }
    for (uint32_t r = 0; r < MM_MAX_TASKS; r++) {
        const struct mm_tree *g = &m->tree[r];
        if (!g->live) {
            if (g->tasks != 0 || g->pages != 0 || g->reserve != 0) {
                FAULT("mm: free tree record %u carries state", r);
            }
            continue;
        }
        if (g->tasks != ttasks[r] || g->pages != tpages[r]) {
            FAULT("mm: tree %u counts %u tasks, %llu pages; has %u, %llu", r, g->tasks,
                  (unsigned long long)g->pages, ttasks[r], (unsigned long long)tpages[r]);
        }
        if ((g->tasks == 0 && g->reserve != 0) || (g->tasks == 0 && g->pages == 0)) {
            FAULT("mm: tree %u has no tasks but keeps a reservation, or is kept for nothing", r);
        }
    }
    if (m->nfree + m->lost + charged != m->npages || m->nfree < unused_all(m)) {
        FAULT("mm: %u free, %llu lost, %llu charged of %u pages; %llu reserved unused", m->nfree,
              (unsigned long long)m->lost, (unsigned long long)charged, m->npages,
              (unsigned long long)unused_all(m));
    }
    uint32_t listed = 0;
    for (uint32_t p = m->free; p != 0 && listed <= m->npages; p = m->pg[p - 1u].next) {
        if (p > m->npages || m->pg[p - 1u].refs != 0 || m->pg[p - 1u].kreg != 0) {
            FAULT("mm: the free list names page %u, in use or past the pool", p - 1u);
            break;
        }
        listed++;
    }
    if (listed != nfree) {
        FAULT("mm: the free list holds %u pages of %u free", listed, nfree);
    }
    return bad;
}
