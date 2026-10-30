/* map.c - frames, page tables and mappings (Phase 3 step 3, Phase 4 step 2;
 * decisions 0013, 0020, 0021; invariant I12).
 *
 * A frame object names a range of physical pages. The kernel makes frames
 * for free memory at boot and gives them to the root task (0013); frames
 * never overlap, so no page is handed out twice. A space's page tables are
 * paid for with frames lent to it (0021), and what is mapped is recorded in
 * a tree that mirrors the hardware's levels (core.h, *page tables*): this
 * file decides every entry, and the kernel's page-table code
 * (kernel/mem/vm.c) writes the hardware's through the hooks. A mapping is
 * any run of a frame's pages (0020); each frame counts the entries naming
 * it. A frame whose last capability goes while it is mapped is dying, and a
 * sweep on the destruction work list (task.c) unmaps it everywhere. Every
 * call over pages covers at most one last-level table. */
#include "internal.h"

_Static_assert(JOIST_MAX_OBJECTS <= 0xffffu, "a page entry names its frame in 16 bits");
_Static_assert(JOIST_MAP_SPAN == JOIST_PT_ENTRIES, "a run lies within one last-level table");
_Static_assert(JOIST_PT_BITS >= 1u && JOIST_PT_ENTRIES * sizeof(uint64_t) <= JOIST_PAGE_SIZE,
               "a node fits a page");
_Static_assert(JOIST_PT_LEVELS == 3u, "the walks below are written for three levels of nodes");

/* ---- the tree ---- */

/* The entry of va's node at `level` (0 the last) for the level below. */
static unsigned idx(uint64_t va, unsigned level)
{
    return (unsigned)((va >> JOIST_PT_SHIFT(level)) & (JOIST_PT_ENTRIES - 1u));
}

static uint64_t *node(const struct joist_kernel *k, uint64_t pfn)
{
    return (uint64_t *)k->hooks.page(k->hooks.ctx, pfn);
}

/* The entry naming the node at `level` that covers va in space s: in the top
 * level, or in the node above. NULL if a node above it is absent. */
static uint64_t *link(const struct joist_kernel *k, uint32_t s, uint64_t va, unsigned level)
{
    uint64_t *e = (uint64_t *)&k->cs[s].pt_top[va >> JOIST_PT_SHIFT(JOIST_PT_LEVELS)];
    for (unsigned l = JOIST_PT_LEVELS - 1u; l > level; l--) {
        if (*e == 0) {
            return NULL;
        }
        e = &node(k, JOIST_PT_SHADOW(*e))[idx(va, l)];
    }
    return e;
}

/* The last-level node covering va (its JOIST_PT_ENTRIES entries), or NULL. */
static uint64_t *leaf(const struct joist_kernel *k, uint32_t s, uint64_t va)
{
    const uint64_t *e = link(k, s, va, 0);
    return e == NULL || *e == 0 ? NULL : node(k, JOIST_PT_SHADOW(*e));
}

/* Pages a run at va needs for the tables it lacks: two per table. */
static uint64_t pages_needed(const struct joist_kernel *k, uint32_t s, uint64_t va)
{
    for (unsigned l = JOIST_PT_LEVELS; l-- > 0;) {
        const uint64_t *e = link(k, s, va, l);
        if (e == NULL || *e == 0) {
            return 2u * ((uint64_t)l + 1u);
        }
    }
    return 0;
}

/* ---- lent pages (0021) ---- */

/* A page for a table of space s, zeroed. The caller has checked pt_avail. */
static uint64_t take_page(struct joist_kernel *k, uint32_t s)
{
    struct joist_cspace_state *st = &k->cs[s];
    uint64_t pfn;
    if (st->pt_free != 0) {
        pfn = st->pt_free - 1u;
        st->pt_free = node(k, pfn)[0];
    } else {
        struct joist_object *f = &k->obj[st->lent_first - 1u];
        pfn = f->base + f->water++;
        if (f->water == f->npages) {
            st->lent_first = f->lent_next; /* full: off the list of frames with room */
            f->lent_next = 0;
        }
    }
    memset(node(k, pfn), 0, JOIST_PT_ENTRIES * sizeof(uint64_t));
    st->pt_avail--;
    st->pt_used++;
    return pfn;
}

static void give_page(struct joist_kernel *k, uint32_t s, uint64_t pfn)
{
    struct joist_cspace_state *st = &k->cs[s];
    node(k, pfn)[0] = st->pt_free;
    st->pt_free = pfn + 1u;
    st->pt_avail++;
    st->pt_used--;
}

/* Make the tables a run at va needs (pages_needed of them checked). */
static void build(struct joist_kernel *k, uint32_t s, uint64_t va)
{
    for (unsigned l = JOIST_PT_LEVELS; l-- > 0;) {
        uint64_t *e = link(k, s, va, l);
        if (*e == 0) {
            uint64_t sh = take_page(k, s);
            uint64_t hw = take_page(k, s);
            *e = JOIST_PT_NODE(sh, hw);
            if (k->hooks.table_new != NULL) {
                k->hooks.table_new(k->hooks.ctx, s, va, l, hw);
            }
        }
    }
}

/* Free the tables covering va that are empty, bottom up. */
static void prune(struct joist_kernel *k, uint32_t s, uint64_t va)
{
    for (unsigned l = 0; l < JOIST_PT_LEVELS; l++) {
        uint64_t *e = link(k, s, va, l);
        if (e == NULL || *e == 0) {
            return;
        }
        const uint64_t *n = node(k, JOIST_PT_SHADOW(*e));
        for (unsigned i = 0; i < JOIST_PT_ENTRIES; i++) {
            JOIST_WORK(k);
            if (n[i] != 0) {
                return;
            }
        }
        if (k->hooks.table_gone != NULL) {
            k->hooks.table_gone(k->hooks.ctx, s, va, l);
        }
        uint64_t sh = JOIST_PT_SHADOW(*e);
        uint64_t hw = JOIST_PT_HW(*e);
        *e = 0;
        give_page(k, s, sh);
        give_page(k, s, hw);
    }
}

/* ---- frames ---- */

/* The rest of a frame's freeing, once nothing maps it or borrows it: its
 * last capability went long ago (dying). */
/* A frame freed: its quota unit back and its fields cleared. Nothing waits
 * on a frame or is notified of it (the IPC and notification checks: no
 * queue, no notification state), and it has no lifecycle, so the rest of
 * joist__obj_free would do nothing. */
static void frame_release(struct joist_kernel *k, uint32_t obj)
{
    joist__obj_clear(k, obj);
}

/* A frame the sweep unmaps: dying (its last capability gone) or dead
 * (killed, 0022, its capabilities still held). */
static int condemned(const struct joist_object *o)
{
    return o->dying || o->dead;
}

/* An entry naming frame f has gone: once a condemned frame is mapped
 * nowhere the sweep is done with it, and a dying one is freed. */
static void frame_unmapped(struct joist_kernel *k, uint32_t f)
{
    struct joist_object *o = &k->obj[f];
    o->mapcount--;
    if (condemned(o) && o->mapcount == 0) {
        k->frames_dying--;
        if (o->dying && o->lent == 0 && o->extents == 0) {
            frame_release(k, f);
        }
    }
}

void joist__frame_extent_gone(struct joist_kernel *k, uint32_t f)
{
    struct joist_object *o = &k->obj[f];
    o->extents--;
    if (o->dying && o->mapcount == 0 && o->lent == 0 && o->extents == 0) {
        frame_release(k, f);
    }
}

int joist__frame_last_ref(struct joist_kernel *k, uint32_t obj)
{
    struct joist_object *o = &k->obj[obj];
    if (o->type != JOIST_OBJ_FRAME || (o->mapcount == 0 && o->lent == 0 && o->extents == 0)) {
        return 0;
    }
    if (o->mapcount != 0 && !o->dead) {
        k->frames_dying++; /* the sweep (joist__frames_sweep) will unmap it */
        k->fround = 1;
    }
    o->dying = 1; /* (a killed frame is counted from its kill) */
    return 1;
}

void joist__frame_killed(struct joist_kernel *k, uint32_t obj)
{
    if (k->obj[obj].mapcount != 0) {
        k->frames_dying++;
        k->fround = 1;
    }
}

/* A new object of `type` in slot d of space cs, from cs's quota: the shared
 * end of boot_frame and frame_split. */
static joist_err new_frame(struct joist_kernel *k, uint32_t cs, struct joist_cap *d, uint64_t base,
                           uint64_t npages, joist_rights rights)
{
    struct joist_cspace_state *s = &k->cs[cs];
    if (s->quota == 0) {
        return JOIST_E_NOMEM;
    }
    uint32_t i;
    if (!joist__obj_peek(k, &i)) {
        return JOIST_E_NOMEM;
    }
    joist__obj_take(k, i);
    struct joist_object *o = &k->obj[i];
    memset(o, 0, sizeof *o);
    o->type = JOIST_OBJ_FRAME;
    o->refs = 1;
    o->donor = cs;
    o->base = base;
    o->npages = npages;
    s->quota--;
    s->charged++;
    k->objects_live++;
    *d = (struct joist_cap){.obj = i + 1u, .type = JOIST_OBJ_FRAME, .rights = rights, .badge = 0};
    joist__cap_added(k, d);
    return JOIST_OK;
}

joist_err joist_boot_frame(struct joist_kernel *k, joist_cspace cs, joist_slot dst, uint64_t base,
                           uint64_t npages)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    if (k->root != cs + 1u) {
        return JOIST_E_STATE; /* all memory starts with the root task (0013) */
    }
    struct joist_cap *d = joist__slot(k, cs, dst);
    if (d == NULL) {
        return JOIST_E_RANGE;
    }
    if (d->obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    if (npages == 0 || npages > JOIST_FRAME_PAGES_MAX || base > UINT64_MAX - npages) {
        return JOIST_E_RANGE;
    }
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        JOIST_WORK(k);
        const struct joist_object *o = &k->obj[i];
        if (o->type == JOIST_OBJ_FRAME && base < o->base + o->npages && o->base < base + npages) {
            return JOIST_E_STATE; /* a page is never in two frames (I12) */
        }
    }
    return new_frame(k, cs, d, base, npages,
                     (joist_rights)(JOIST_R_CONTROL | JOIST_R_WRITE | JOIST_R_KILL | JOIST_R_GRANT));
}

joist_err joist_boot_self(struct joist_kernel *k, joist_cspace cs, joist_slot dst)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    if (k->root != cs + 1u || k->cs[cs].task_obj != 0) {
        return JOIST_E_STATE;
    }
    return joist__task_cap_new(k, cs, dst, cs);
}

joist_err joist_frame_split(struct joist_kernel *k, joist_cspace cs, joist_slot slot, uint64_t at,
                            joist_slot dst)
{
    uint32_t fo;
    joist_err e = joist__control_cap(k, cs, slot, JOIST_OBJ_FRAME, &fo);
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_object *f = &k->obj[fo];
    if (at == 0 || at >= f->npages) {
        return JOIST_E_RANGE;
    }
    struct joist_cap *d = joist__slot(k, cs, dst);
    if (d == NULL) {
        return JOIST_E_RANGE;
    }
    if (d->obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    /* Only an unshared, unmapped, unlent frame: otherwise someone's view of
     * it, a mapping of it or a space's tables in it would change under them. */
    if (f->refs != 1 || f->mapcount != 0 || f->lent != 0 || f->extents != 0) {
        return JOIST_E_STATE; /* (or a region's extent's window would move) */
    }
    joist_rights rights = joist__slot_c(k, cs, slot)->rights;
    uint64_t rest = f->npages - at;
    e = new_frame(k, cs, d, f->base + at, rest, rights);
    if (e == JOIST_OK) {
        f->npages = at;
    }
    return e;
}

/* The space of the task in task_slot (CONTROL), in *space. */
static joist_err task_space(const struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                            uint32_t *space)
{
    uint32_t to;
    joist_err e = joist__control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &to);
    if (e == JOIST_OK) {
        *space = k->obj[to].target - 1u;
    }
    return e;
}

joist_err joist_space_donate(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                             joist_slot frame_slot)
{
    uint32_t fo;
    uint32_t space;
    joist_err e = joist__control_cap(k, cs, frame_slot, JOIST_OBJ_FRAME, &fo);
    if (e == JOIST_OK) {
        e = task_space(k, cs, task_slot, &space);
    }
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_object *f = &k->obj[fo];
    if (f->mapcount != 0 || f->lent != 0 || f->dying || f->extents != 0) {
        return JOIST_E_STATE; /* never a mapped frame: a task could write its tables */
    }
    if (k->hooks.page == NULL || f->base + f->npages > JOIST_PT_PFN_LIMIT ||
        k->hooks.page(k->hooks.ctx, f->base) == NULL ||
        k->hooks.page(k->hooks.ctx, f->base + f->npages - 1u) == NULL) {
        return JOIST_E_STATE; /* pages the core can't reach can't hold its tables */
    }
    struct joist_cspace_state *st = &k->cs[space];
    f->lent = space + 1u;
    f->water = 0;
    f->lent_next = st->lent_first;
    st->lent_first = fo + 1u;
    st->pt_avail += f->npages;
    return JOIST_OK;
}

/* ---- mapping ---- */

static int bad_perms(unsigned perms)
{
    return (perms & ~(unsigned)(JOIST_MAP_W | JOIST_MAP_X)) != 0 ||
           ((perms & JOIST_MAP_W) && (perms & JOIST_MAP_X)); /* W^X, always */
}

/* A run [va, va + npages) inside the user half and one last-level table. */
static int bad_run(uint64_t va, uint64_t npages)
{
    return npages == 0 || va >= JOIST_USER_PAGES || npages > JOIST_USER_PAGES - va ||
           npages > JOIST_MAP_SPAN - va % JOIST_MAP_SPAN;
}

static joist_err map_run(struct joist_kernel *k, joist_cspace cs, joist_slot frame_slot,
                         uint64_t off, uint64_t npages, joist_slot task_slot, uint64_t va,
                         unsigned perms, int replace, joist_slot old_slot, uint64_t old_off)
{
    uint32_t fo;
    uint32_t space;
    uint32_t oo = 0;
    joist_err e = joist__control_cap(k, cs, frame_slot, JOIST_OBJ_FRAME, &fo);
    if (e == JOIST_OK) {
        e = task_space(k, cs, task_slot, &space);
    }
    if (e == JOIST_OK && replace) {
        e = joist__control_cap(k, cs, old_slot, JOIST_OBJ_FRAME, &oo);
    }
    if (e != JOIST_OK) {
        return e;
    }
    int ceil_w = (joist__slot_c(k, cs, frame_slot)->rights & JOIST_R_WRITE) != 0;
    if (bad_perms(perms) || ((perms & JOIST_MAP_W) && !ceil_w)) {
        return JOIST_E_RIGHTS;
    }
    struct joist_object *f = &k->obj[fo];
    if (bad_run(va, npages) || off >= f->npages || npages > f->npages - off) {
        return JOIST_E_RANGE;
    }
    if (f->lent != 0 || f->dying || joist__region_meets(k, space, va, npages)) {
        return JOIST_E_STATE; /* (inside a region only region calls map: 0030) */
    }
    if (!replace) {
        return joist__map_pages(k, space, fo, off, npages, va, perms, ceil_w);
    }
    uint64_t *lf = leaf(k, space, va);
    if (lf == NULL || old_off > UINT64_MAX - npages) {
        return JOIST_E_STATE;
    }
    for (uint64_t i = 0; i < npages; i++) {
        JOIST_WORK(k);
        uint64_t pe = lf[(va + i) % JOIST_PT_ENTRIES];
        if (pe == 0 || JOIST_PTE_FRAME(pe) != oo || JOIST_PTE_OFF(pe) != old_off + i) {
            return JOIST_E_STATE; /* not what the caller expected: nothing changes */
        }
    }
    for (uint64_t i = 0; i < npages; i++) {
        JOIST_WORK(k);
        lf[(va + i) % JOIST_PT_ENTRIES] = JOIST_PTE(fo, off + i, perms, ceil_w);
    }
    k->obj[oo].mapcount -= npages; /* oo has a capability: it can't be dying */
    f->mapcount += npages;
    if (k->hooks.map != NULL) {
        k->hooks.map(k->hooks.ctx, space, va, f->base + off, npages, perms, 1);
    }
    return JOIST_OK;
}

joist_err joist__map_pages(struct joist_kernel *k, uint32_t space, uint32_t fo, uint64_t off,
                           uint64_t npages, uint64_t va, unsigned perms, int ceil_w)
{
    struct joist_object *f = &k->obj[fo];
    uint64_t *lf = leaf(k, space, va);
    for (uint64_t i = 0; lf != NULL && i < npages; i++) {
        JOIST_WORK(k);
        if (lf[(va + i) % JOIST_PT_ENTRIES] != 0) {
            return JOIST_E_OCCUPIED;
        }
    }
    if (k->cs[space].pt_avail < pages_needed(k, space, va)) {
        return JOIST_E_NOMEM; /* lend the space more first (0021) */
    }
    build(k, space, va);
    lf = leaf(k, space, va);
    for (uint64_t i = 0; i < npages; i++) {
        JOIST_WORK(k);
        lf[(va + i) % JOIST_PT_ENTRIES] = JOIST_PTE(fo, off + i, perms, ceil_w);
    }
    f->mapcount += npages;
    if (k->hooks.map != NULL) {
        k->hooks.map(k->hooks.ctx, space, va, f->base + off, npages, perms, 0);
    }
    return JOIST_OK;
}

joist_err joist_map(struct joist_kernel *k, joist_cspace cs, joist_slot frame_slot, uint64_t off,
                    uint64_t npages, joist_slot task_slot, uint64_t va, unsigned perms)
{
    return map_run(k, cs, frame_slot, off, npages, task_slot, va, perms, 0, JOIST_SLOT_NONE, 0);
}

joist_err joist_map_replace(struct joist_kernel *k, joist_cspace cs, joist_slot frame_slot,
                            uint64_t off, uint64_t npages, joist_slot task_slot, uint64_t va,
                            unsigned perms, joist_slot old_slot, uint64_t old_off)
{
    return map_run(k, cs, frame_slot, off, npages, task_slot, va, perms, 1, old_slot, old_off);
}

/* Clear the entries of lf (va's last-level node in space s) in [va, va + n)
 * that name a frame `which` selects (all, or the dying ones), telling the
 * kernel run by run if `tell`. Returns how many it cleared. */
enum { CLEAR_ALL, CLEAR_DYING };
static uint64_t clear_run(struct joist_kernel *k, uint32_t s, uint64_t *lf, uint64_t va, uint64_t n,
                      int which, int tell)
{
    uint64_t run = 0; /* pages in the run being cleared, ending before va + i */
    uint64_t cleared = 0;
    for (uint64_t i = 0; i <= n; i++) {
        JOIST_WORK(k);
        uint64_t pe = i < n ? lf[(va + i) % JOIST_PT_ENTRIES] : 0;
        int go = pe != 0 && (which == CLEAR_ALL || condemned(&k->obj[JOIST_PTE_FRAME(pe)]));
        if (go) {
            lf[(va + i) % JOIST_PT_ENTRIES] = 0;
            run++;
            cleared++;
        }
        if (!go && run != 0) {
            if (tell && k->hooks.unmap != NULL) {
                k->hooks.unmap(k->hooks.ctx, s, va + i - run, run);
            }
            run = 0;
        }
        if (go) {
            frame_unmapped(k, JOIST_PTE_FRAME(pe)); /* after the run is told: see below */
        }
    }
    return cleared;
}

joist_err joist_unmap_range(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                            uint64_t va, uint64_t npages)
{
    uint32_t space;
    joist_err e = task_space(k, cs, task_slot, &space);
    if (e != JOIST_OK) {
        return e;
    }
    if (bad_run(va, npages)) {
        return JOIST_E_RANGE;
    }
    if (joist__region_meets(k, space, va, npages)) {
        return JOIST_E_STATE; /* inside a region only region calls unmap (0030) */
    }
    joist__unmap_pages(k, space, va, npages);
    return JOIST_OK;
}

void joist__unmap_pages(struct joist_kernel *k, uint32_t space, uint64_t va, uint64_t npages)
{
    uint64_t *lf = leaf(k, space, va);
    if (lf != NULL) {
        (void)clear_run(k, space, lf, va, npages, CLEAR_ALL, 1);
        prune(k, space, va);
    }
}

joist_err joist_protect(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot, uint64_t va,
                        uint64_t npages, unsigned perms)
{
    uint32_t space;
    joist_err e = task_space(k, cs, task_slot, &space);
    if (e != JOIST_OK) {
        return e;
    }
    if (bad_perms(perms)) {
        return JOIST_E_RIGHTS;
    }
    if (bad_run(va, npages)) {
        return JOIST_E_RANGE;
    }
    if (joist__region_meets(k, space, va, npages)) {
        return JOIST_E_STATE; /* inside a region only region calls protect (0030) */
    }
    return joist__protect_pages(k, space, va, npages, perms);
}

joist_err joist__protect_pages(struct joist_kernel *k, uint32_t space, uint64_t va,
                               uint64_t npages, unsigned perms)
{
    uint64_t *lf = leaf(k, space, va);
    if (lf == NULL) {
        return JOIST_OK;
    }
    for (uint64_t i = 0; (perms & JOIST_MAP_W) && i < npages; i++) {
        JOIST_WORK(k);
        uint64_t pe = lf[(va + i) % JOIST_PT_ENTRIES];
        if (pe != 0 && (pe & JOIST_PTE_CEIL_W) == 0) {
            return JOIST_E_RIGHTS; /* beyond the rights ceiling: nothing changes */
        }
    }
    uint64_t run = 0;
    for (uint64_t i = 0; i <= npages; i++) {
        JOIST_WORK(k);
        uint64_t *pe = i < npages ? &lf[(va + i) % JOIST_PT_ENTRIES] : NULL;
        if (pe != NULL && *pe != 0) {
            *pe = (*pe & ~(uint64_t)(JOIST_PTE_W | JOIST_PTE_X)) | perms;
            run++;
        } else if (run != 0) {
            if (k->hooks.protect != NULL) {
                k->hooks.protect(k->hooks.ctx, space, va + i - run, run, perms);
            }
            run = 0;
        }
    }
    return JOIST_OK;
}

joist_err joist_map_query(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                          uint64_t va, uint64_t npages, joist_slot frame_slot, uint64_t *out)
{
    uint32_t space;
    joist_err e = task_space(k, cs, task_slot, &space);
    uint32_t fo = JOIST_MAX_OBJECTS; /* no frame named */
    if (e == JOIST_OK && frame_slot != JOIST_SLOT_NONE) {
        e = joist__control_cap(k, cs, frame_slot, JOIST_OBJ_FRAME, &fo); /* the frame named */
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (npages > JOIST_QUERY_MAX || bad_run(va, npages)) {
        return JOIST_E_RANGE;
    }
    const uint64_t *lf = leaf(k, space, va);
    for (uint64_t i = 0; i < npages; i++) {
        JOIST_WORK(k);
        uint64_t pe = lf != NULL ? lf[(va + i) % JOIST_PT_ENTRIES] : 0;
        uint64_t q = 0;
        if (pe != 0) {
            q = JOIST_QUERY_PRESENT | JOIST_PTE_PERMS(pe) |
                ((pe & JOIST_PTE_CEIL_W) != 0 ? (uint64_t)JOIST_QUERY_CEIL_W : 0u);
            if (JOIST_PTE_FRAME(pe) == fo) {
                q |= JOIST_QUERY_FRAME | (JOIST_PTE_OFF(pe) << 32);
            }
        }
        out[i] = q;
    }
    return JOIST_OK;
}

int joist__bad_perms(unsigned perms)
{
    return bad_perms(perms);
}

int joist__bad_run(uint64_t va, uint64_t npages)
{
    return bad_run(va, npages);
}

int joist__has_table(const struct joist_kernel *k, uint32_t space, uint64_t va)
{
    return leaf(k, space, va) != NULL;
}

uint64_t joist__pte(const struct joist_kernel *k, uint32_t space, uint64_t va)
{
    const uint64_t *lf = leaf(k, space, va);
    return lf != NULL ? lf[va % JOIST_PT_ENTRIES] : 0;
}

/* ---- sweeps (the destruction work list, task.c) ---- */

/* The next last-level table to look at from `t` (in tables of the space),
 * skipping the absent parts of the tree: JOIST_PT_SWEEP_SPACE when past the
 * end. *lf is its node, or NULL if t itself is absent. */
static uint64_t sweep_next(const struct joist_kernel *k, uint32_t s, uint64_t t, uint64_t **lf)
{
    *lf = NULL;
    uint64_t va = t * JOIST_PT_ENTRIES;
    const uint64_t *e2 = link(k, s, va, 2);
    if (*e2 == 0) {
        /* the next top-level entry's first table */
        return ((va >> JOIST_PT_SHIFT(3)) + 1u) << (JOIST_PT_SHIFT(3) - JOIST_PT_BITS);
    }
    const uint64_t *e1 = link(k, s, va, 1);
    if (*e1 == 0) {
        return ((va >> JOIST_PT_SHIFT(2)) + 1u) << (JOIST_PT_SHIFT(2) - JOIST_PT_BITS);
    }
    *lf = leaf(k, s, va);
    return t + 1u;
}

int joist__space_tables_sweep(struct joist_kernel *k, uint32_t s, uint32_t *done)
{
    struct joist_cspace_state *st = &k->cs[s];
    while (st->tsweep < JOIST_PT_SWEEP_SPACE) {
        JOIST_WORK(k);
        if (k->preempt != 0 && *done >= k->preempt) {
            return 1;
        }
        (*done)++;
        uint64_t *lf;
        uint64_t next = sweep_next(k, s, st->tsweep, &lf);
        if (lf != NULL) {
            /* A dying space: its hardware tables go with space_gone. */
            (void)clear_run(k, s, lf, st->tsweep * JOIST_PT_ENTRIES, JOIST_PT_ENTRIES, CLEAR_ALL, 0);
        }
        st->tsweep = next;
    }
    return 0;
}

void joist__space_tables_gone(struct joist_kernel *k, uint32_t s)
{
    struct joist_cspace_state *st = &k->cs[s];
    for (unsigned i = 0; i < JOIST_PT_TOP; i++) {
        st->pt_top[i] = 0; /* every node is in a frame lent to s, returned below */
    }
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        JOIST_WORK(k);
        struct joist_object *f = &k->obj[o];
        if (f->type == JOIST_OBJ_FRAME && f->lent == s + 1u) {
            f->lent = 0;
            f->lent_next = 0;
            f->water = 0;
            if (f->dying && f->mapcount == 0 && f->extents == 0) {
                frame_release(k, o); /* its capabilities went while it was lent */
            }
        }
    }
    st->lent_first = 0;
    st->pt_free = 0;
    st->pt_avail = 0;
    st->pt_used = 0;
    st->tsweep = 0;
}

int joist__frames_sweep(struct joist_kernel *k, uint32_t *done)
{
    while (k->frames_dying != 0) {
        JOIST_WORK(k);
        if (k->preempt != 0 && *done >= k->preempt) {
            return 1;
        }
        (*done)++;
        uint32_t s = (uint32_t)(k->fsweep / JOIST_PT_SWEEP_SPACE);
        uint64_t t = k->fsweep % JOIST_PT_SWEEP_SPACE;
        if (s >= JOIST_MAX_CSPACES) {
            if (!k->fround) {
                /* A whole round found nothing, and no frame began dying
                 * during it: frames_dying is wrong (I12). Stop rather than
                 * go round for ever; the checker reports it. */
                return 0;
            }
            k->fround = 0;
            k->fsweep = 0; /* round again: frames that began dying meanwhile */
            continue;
        }
        if (!k->cs[s].live) {
            k->fsweep = (uint64_t)(s + 1u) * JOIST_PT_SWEEP_SPACE;
            continue;
        }
        uint64_t *lf;
        uint64_t next = sweep_next(k, s, t, &lf);
        if (lf != NULL) {
            int live = !k->cs[s].dying;
            if (clear_run(k, s, lf, t * JOIST_PT_ENTRIES, JOIST_PT_ENTRIES, CLEAR_DYING, live) != 0) {
                k->fround = 1;
            }
            if (live) {
                prune(k, s, t * JOIST_PT_ENTRIES);
            }
        }
        k->fsweep = next >= JOIST_PT_SWEEP_SPACE ? (uint64_t)(s + 1u) * JOIST_PT_SWEEP_SPACE
                                                 : (uint64_t)s * JOIST_PT_SWEEP_SPACE + next;
    }
    return 0;
}

/* ---- invariants (I12, and I3's share of page tables) ---- */

/* Frames lent to s holding page pfn: its index plus one, or 0. Only below
 * a sound water mark, so a damaged frame never leads the check past its
 * pages. */
static uint32_t lent_holding(const struct joist_kernel *k, uint32_t s, uint64_t pfn)
{
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *f = &k->obj[o];
        if (f->type == JOIST_OBJ_FRAME && f->lent == s + 1u && f->water <= f->npages &&
            pfn >= f->base && pfn < f->base + f->water) {
            return o + 1u;
        }
    }
    return 0;
}

/* Sort the n frames in f by their first page: a heap sort, in place, with
 * no recursion and loops bounded by n (CBMC unwinds them as the others). */
static void sift(const struct joist_kernel *k, uint32_t *f, uint32_t at, uint32_t n)
{
    for (uint32_t depth = 0; depth < 32u; depth++) {
        uint32_t big = at;
        uint32_t l = 2u * at + 1u;
        if (l < n && k->obj[f[l]].base > k->obj[f[big]].base) {
            big = l;
        }
        if (l + 1u < n && k->obj[f[l + 1u]].base > k->obj[f[big]].base) {
            big = l + 1u;
        }
        if (big == at) {
            return;
        }
        uint32_t t = f[at];
        f[at] = f[big];
        f[big] = t;
        at = big;
    }
}

static void sort_by_base(const struct joist_kernel *k, uint32_t *f, uint32_t n)
{
    for (uint32_t i = n / 2u; i-- > 0;) {
        sift(k, f, i, n);
    }
    for (uint32_t end = n; end > 1u; end--) {
        uint32_t t = f[0];
        f[0] = f[end - 1u];
        f[end - 1u] = t;
        sift(k, f, 0, end - 1u);
    }
}

unsigned joist__check_map(const struct joist_kernel *k, char *msg, unsigned msglen, int msg_free)
{
    unsigned bad = 0;
#define VIOLATION(...)                                                                             \
    do {                                                                                           \
        JOIST_CBMC_NOTE(__LINE__);                                                                 \
        if (bad++ == 0 && msg_free && msg != NULL && msglen > 0) {                                 \
            JOIST_FMT(msg, msglen, __VA_ARGS__);                                                   \
        }                                                                                          \
    } while (0)

    /* Under the lock, one checker at a time: too large for a kernel stack. */
    static uint64_t seen[JOIST_MAX_OBJECTS];
    static uint32_t byb[JOIST_MAX_OBJECTS]; /* the sound frames, to sort by base */
    uint32_t nfr = 0;
    uint32_t dying_mapped = 0;
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        seen[i] = 0;
        const struct joist_object *a = &k->obj[i];
        if (a->type != JOIST_OBJ_FRAME) {
            if (a->mapcount != 0 || a->lent != 0 || a->lent_next != 0 || a->water != 0 || a->dying) {
                VIOLATION("I12: object %u is no frame but carries a frame's state", i);
            }
            continue;
        }
        if (a->npages == 0 || a->npages > JOIST_FRAME_PAGES_MAX || a->base > UINT64_MAX - a->npages) {
            VIOLATION("I12: frame %u is empty, too large or wraps", i);
            continue;
        }
        if (a->dying != (a->refs == 0)) {
            VIOLATION("I12: frame %u dying %u with %u capabilities", i, a->dying, a->refs);
        }
        if (a->dying && a->mapcount == 0 && a->lent == 0 && a->extents == 0) {
            VIOLATION("I12: frame %u dying, but nothing keeps it", i);
        }
        dying_mapped += (uint32_t)(condemned(a) && a->mapcount != 0);
        if (a->dead && a->lent != 0) {
            VIOLATION("0022: frame %u killed while lent", i);
        }
        if (a->lent != 0) {
            if (a->lent > JOIST_MAX_CSPACES || !k->cs[a->lent - 1u].live) {
                VIOLATION("I12: frame %u lent to a dead or invalid space", i);
            }
            if (a->mapcount != 0 || a->water > a->npages) {
                VIOLATION("I12: frame %u lent and mapped, or past its pages", i);
            }
        } else if (a->water != 0 || a->lent_next != 0) {
            VIOLATION("I12: frame %u not lent but carries lending state", i);
        }
        byb[nfr++] = i; /* sound: its overlaps are checked below */
    }
    /* No two frames overlap: in order of their first pages, none starts
     * before the furthest end of those before it. O(n log n), not n^2: the
     * checking kernel runs this after every call (choice 10's 2 048 objects
     * made the pairwise test the cost of the 4-CPU self-tests). */
    sort_by_base(k, byb, nfr);
    uint32_t far = 0; /* the frame ending furthest so far */
    for (uint32_t n = 0; n < nfr; n++) {
        const struct joist_object *b = &k->obj[byb[n]];
        if (n != 0 && b->base < k->obj[far].base + k->obj[far].npages) {
            VIOLATION("I12: frames %u and %u overlap", far, byb[n]);
        }
        if (n == 0 || b->base + b->npages > k->obj[far].base + k->obj[far].npages) {
            far = byb[n];
        }
    }
    if (dying_mapped != k->frames_dying) {
        VIOLATION("I12/0022: %u frames dying or killed while mapped, but frames_dying %u",
                  dying_mapped, k->frames_dying);
    }
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        const struct joist_cspace_state *st = &k->cs[s];
        uint64_t nodes = 0;
        uint64_t room = 0;
        if (!st->live) {
            for (unsigned i = 0; i < JOIST_PT_TOP; i++) {
                if (st->pt_top[i] != 0) {
                    VIOLATION("I12: dead space %u has page tables", s);
                    break;
                }
            }
            if (st->lent_first != 0 || st->pt_free != 0 || st->pt_avail != 0 || st->pt_used != 0) {
                VIOLATION("I12: dead space %u has lent pages", s);
            }
            continue;
        }
        /* The tree: every node in a page lent to s; every entry sound. */
        for (unsigned t2 = 0; t2 < JOIST_PT_TOP; t2++) {
            uint64_t e2 = st->pt_top[t2];
            if (e2 == 0) {
                continue;
            }
            const uint64_t *n2 = NULL;
            nodes++;
            if (lent_holding(k, s, JOIST_PT_SHADOW(e2)) == 0 || lent_holding(k, s, JOIST_PT_HW(e2)) == 0) {
                VIOLATION("I12: space %u has a table outside its lent pages", s);
                continue;
            }
            n2 = node(k, JOIST_PT_SHADOW(e2));
            for (unsigned t1 = 0; t1 < JOIST_PT_ENTRIES; t1++) {
                uint64_t e1 = n2[t1];
                if (e1 == 0) {
                    continue;
                }
                nodes++;
                if (lent_holding(k, s, JOIST_PT_SHADOW(e1)) == 0 ||
                    lent_holding(k, s, JOIST_PT_HW(e1)) == 0) {
                    VIOLATION("I12: space %u has a table outside its lent pages", s);
                    continue;
                }
                const uint64_t *n1 = node(k, JOIST_PT_SHADOW(e1));
                for (unsigned t0 = 0; t0 < JOIST_PT_ENTRIES; t0++) {
                    uint64_t e0 = n1[t0];
                    if (e0 == 0) {
                        continue;
                    }
                    nodes++;
                    if (lent_holding(k, s, JOIST_PT_SHADOW(e0)) == 0 ||
                        lent_holding(k, s, JOIST_PT_HW(e0)) == 0) {
                        VIOLATION("I12: space %u has a table outside its lent pages", s);
                        continue;
                    }
                    const uint64_t *lf = node(k, JOIST_PT_SHADOW(e0));
                    unsigned present = 0;
                    for (unsigned i = 0; i < JOIST_PT_ENTRIES; i++) {
                        present += lf[i] != 0;
                    }
                    /* An emptied table is freed (prune) but in a dying
                     * space, whose sweep frees them all at its end: so a
                     * range with no table has nothing mapped (0030 relies
                     * on it). */
                    if (present == 0 && !st->dying) {
                        VIOLATION("I12: space %u has an empty last-level table", s);
                    }
                    for (unsigned i = 0; i < JOIST_PT_ENTRIES; i++) {
                        uint64_t pe = lf[i];
                        if (pe == 0) {
                            continue;
                        }
                        uint32_t fr = JOIST_PTE_FRAME(pe);
                        unsigned p = JOIST_PTE_PERMS(pe);
                        if ((pe & JOIST_PTE_P) == 0 || (pe & 0xfff0u) != 0 || fr >= JOIST_MAX_OBJECTS ||
                            k->obj[fr].type != JOIST_OBJ_FRAME || k->obj[fr].lent != 0 ||
                            JOIST_PTE_OFF(pe) >= k->obj[fr].npages) {
                            VIOLATION("I12: space %u page %llu names no page of a mappable frame", s,
                                      (unsigned long long)(((uint64_t)t2 << JOIST_PT_SHIFT(3)) |
                                                           ((uint64_t)t1 << JOIST_PT_SHIFT(2)) |
                                                           ((uint64_t)t0 << JOIST_PT_SHIFT(1)) | i));
                            continue;
                        }
                        if ((p & JOIST_PTE_W) && (p & JOIST_PTE_X)) {
                            VIOLATION("I12: space %u has a page writable and executable", s);
                        }
                        if ((p & JOIST_PTE_W) && (pe & JOIST_PTE_CEIL_W) == 0) {
                            VIOLATION("I12: space %u has a page writable beyond its ceiling", s);
                        }
                        seen[fr]++;
                    }
                }
            }
        }
        if (st->pt_used != 2u * nodes) {
            VIOLATION("I12: space %u uses %llu table pages, but has %llu tables", s,
                      (unsigned long long)st->pt_used, (unsigned long long)nodes);
        }
        /* Its lent pages: the free list, and the frames with room. */
        uint64_t freepages = 0;
        for (uint64_t p = st->pt_free; p != 0 && freepages <= st->pt_avail; p = node(k, p - 1u)[0]) {
            if (lent_holding(k, s, p - 1u) == 0) {
                VIOLATION("I12: space %u has a free table page outside its lent pages", s);
                break;
            }
            freepages++;
        }
        uint32_t listed = 0;
        for (uint32_t f = st->lent_first; f != 0 && listed <= JOIST_MAX_OBJECTS; f = k->obj[f - 1u].lent_next) {
            const struct joist_object *o = &k->obj[f - 1u];
            if (f > JOIST_MAX_OBJECTS || o->type != JOIST_OBJ_FRAME || o->lent != s + 1u ||
                o->water >= o->npages) {
                VIOLATION("I12: space %u lists a frame with no room as lent to it", s);
                break;
            }
            room += o->npages - o->water;
            listed++;
        }
        uint32_t with_room = 0;
        for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
            with_room += (uint32_t)(k->obj[o].type == JOIST_OBJ_FRAME && k->obj[o].lent == s + 1u &&
                                    k->obj[o].water < k->obj[o].npages);
        }
        if (listed != with_room) {
            VIOLATION("I12: space %u lists %u lent frames with room of %u", s, listed, with_room);
        }
        if (st->pt_avail != freepages + room) {
            VIOLATION("I12: space %u may take %llu pages, but has %llu", s,
                      (unsigned long long)st->pt_avail, (unsigned long long)(freepages + room));
        }
    }
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        if (k->obj[i].type == JOIST_OBJ_FRAME && seen[i] != k->obj[i].mapcount) {
            VIOLATION("I12: frame %u is named by %llu entries but counts %llu", i,
                      (unsigned long long)seen[i], (unsigned long long)k->obj[i].mapcount);
        }
    }
#undef VIOLATION
    return bad;
}
