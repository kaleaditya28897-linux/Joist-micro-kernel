/* region.c - regions: a range of one task's addresses that it maps itself
 * (decision 0030; 0008's escape hatch; Phase 5).
 *
 * A region covers whole last-level tables of one task, at most
 * JOIST_REGION_TABLES of them, and holds up to JOIST_REGION_EXTENTS extents,
 * each a window of one frame. The memory manager makes it, adds extents and
 * kills it; the task, holding JOIST_R_MAP alone, maps pages of the extents
 * anywhere in the range, unmaps and protects them. Inside a region only these
 * calls act, and outside it they can't: plain map, map_replace, unmap_range
 * and protect refuse a run that meets a region (map.c), so the two writers of
 * a task's mappings never meet (0008's warning).
 *
 * Lifetime. The object goes with its last capability, as any object; the
 * record (k->reg) goes when both the object and the range are gone:
 *   - killed (joist_obj_kill) or its last capability deleted: the record is
 *     dying, and the work list unmaps its range a table a step
 *     (joist__regions_sweep), then gives back its extents;
 *   - its task gone: the task's own sweep cleared its tables, and
 *     joist__regions_space_gone gives back the extents and marks the object
 *     dead, so any capability to it fails JOIST_E_GONE.
 * A frame in an extent is kept (its `extents` count) while the extent lasts,
 * so an extent never names a freed or reused object. */
#include "internal.h"

_Static_assert(JOIST_REGION_EXTENTS >= 1u && JOIST_REGIONS_PER_TASK >= 1u &&
                   JOIST_REGION_TABLES >= 1u && JOIST_MAX_REGIONS >= 1u,
               "region sizes");

/* Pages the region's range covers, and its end. */
static uint64_t span(const struct joist_region *r)
{
    return (uint64_t)r->tables * JOIST_PT_ENTRIES;
}

/* Whether [va, va + n) lies inside r's range. */
static int inside(const struct joist_region *r, uint64_t va, uint64_t n)
{
    return va >= r->lo && n <= span(r) && va - r->lo <= span(r) - n;
}

int joist__region_meets(const struct joist_kernel *k, uint32_t space, uint64_t va, uint64_t npages)
{
    const struct joist_cspace_state *st = &k->cs[space];
    for (uint32_t i = 0; i < JOIST_REGIONS_PER_TASK; i++) {
        if (st->region[i] == 0) {
            continue;
        }
        const struct joist_region *r = &k->reg[st->region[i] - 1u];
        if (va < r->lo + span(r) && r->lo < va + npages) {
            return 1;
        }
    }
    return 0;
}

/* The region record named by the capability in cs's slot, which must hold
 * `need`: E_RANGE/E_DEAD (space), E_RANGE, E_EMPTY, E_TYPE, E_RIGHTS, and
 * E_GONE if the region is killed, dying or its task gone or going. */
static joist_err region_cap(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                            joist_rights need, uint32_t *rec)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    if (!joist__usable(k, cs)) {
        return JOIST_E_DEAD;
    }
    const struct joist_cap *c = joist__slot_c(k, cs, slot);
    if (c == NULL) {
        return JOIST_E_RANGE;
    }
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (c->type != JOIST_OBJ_REGION) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & need) != need) {
        return JOIST_E_RIGHTS;
    }
    const struct joist_object *o = &k->obj[c->obj - 1u];
    if (o->dead || o->target == 0) {
        return JOIST_E_GONE;
    }
    const struct joist_region *r = &k->reg[o->target - 1u];
    if (r->dying || r->space == 0 || !joist__usable(k, r->space - 1u)) {
        return JOIST_E_GONE;
    }
    *rec = o->target - 1u;
    return JOIST_OK;
}

joist_err joist_region_create(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                              uint64_t lo, uint32_t tables, unsigned ceil, joist_slot dst)
{
    uint32_t to;
    joist_err e = joist__control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &to);
    if (e != JOIST_OK) {
        return e;
    }
    uint32_t space = k->obj[to].target - 1u;
    if (!joist__usable(k, space)) {
        return JOIST_E_GONE;
    }
    struct joist_cap *d = joist__slot(k, cs, dst);
    if (d == NULL || (ceil & ~(unsigned)(JOIST_MAP_W | JOIST_MAP_X)) != 0 ||
        lo % JOIST_PT_ENTRIES != 0 || tables == 0 || tables > JOIST_REGION_TABLES ||
        lo >= JOIST_USER_PAGES || (uint64_t)tables * JOIST_PT_ENTRIES > JOIST_USER_PAGES - lo) {
        return JOIST_E_RANGE;
    }
    if (d->obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    uint64_t n = (uint64_t)tables * JOIST_PT_ENTRIES;
    if (joist__region_meets(k, space, lo, n)) {
        return JOIST_E_STATE;
    }
    for (uint32_t t = 0; t < tables; t++) {
        JOIST_WORK(k);
        /* A table exists only where something is mapped (an emptied one is
         * freed: I12), so no table means nothing mapped. */
        if (joist__has_table(k, space, lo + (uint64_t)t * JOIST_PT_ENTRIES)) {
            return JOIST_E_STATE;
        }
    }
    struct joist_cspace_state *ts = &k->cs[space];
    uint32_t at = JOIST_REGIONS_PER_TASK;
    for (uint32_t i = 0; i < JOIST_REGIONS_PER_TASK && at == JOIST_REGIONS_PER_TASK; i++) {
        JOIST_WORK(k);
        if (ts->region[i] == 0) {
            at = i;
        }
    }
    uint32_t rec = JOIST_MAX_REGIONS;
    for (uint32_t i = 0; i < JOIST_MAX_REGIONS && rec == JOIST_MAX_REGIONS; i++) {
        JOIST_WORK(k);
        if (!k->reg[i].live) {
            rec = i;
        }
    }
    uint32_t i;
    if (at == JOIST_REGIONS_PER_TASK || rec == JOIST_MAX_REGIONS || k->cs[cs].quota == 0 ||
        !joist__obj_peek(k, &i)) {
        return JOIST_E_NOMEM;
    }
    joist__obj_take(k, i);
    struct joist_object *o = &k->obj[i];
    memset(o, 0, sizeof *o);
    o->type = JOIST_OBJ_REGION;
    o->refs = 1;
    o->donor = cs;
    o->target = rec + 1u;
    k->cs[cs].quota--;
    k->cs[cs].charged++;
    k->objects_live++;
    struct joist_region *r = &k->reg[rec];
    memset(r, 0, sizeof *r);
    r->live = 1;
    r->obj = i + 1u;
    r->space = space + 1u;
    r->lo = lo;
    r->tables = tables;
    r->ceil = (uint8_t)ceil;
    ts->region[at] = rec + 1u;
    *d = (struct joist_cap){.obj = i + 1u,
                            .type = JOIST_OBJ_REGION,
                            .rights = (joist_rights)(JOIST_R_CONTROL | JOIST_R_MAP | JOIST_R_KILL |
                                                     JOIST_R_GRANT),
                            .badge = 0};
    joist__cap_added(k, d);
    return JOIST_OK;
}

joist_err joist_region_add(struct joist_kernel *k, joist_cspace cs, joist_slot region_slot,
                           joist_slot frame_slot, uint64_t off, uint64_t npages, uint32_t *ext)
{
    uint32_t rec;
    uint32_t fo;
    joist_err e = region_cap(k, cs, region_slot, JOIST_R_CONTROL, &rec);
    if (e == JOIST_OK) {
        e = joist__control_cap(k, cs, frame_slot, JOIST_OBJ_FRAME, &fo);
    }
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_object *f = &k->obj[fo];
    if (npages == 0 || off >= f->npages || npages > f->npages - off) {
        return JOIST_E_RANGE;
    }
    if (f->lent != 0) {
        return JOIST_E_STATE; /* its pages are a space's tables (0021) */
    }
    struct joist_region *r = &k->reg[rec];
    if (r->next == JOIST_REGION_EXTENTS) {
        return JOIST_E_NOMEM;
    }
    uint32_t x = r->next++;
    r->ext[x].frame = fo + 1u;
    r->ext[x].off = off;
    r->ext[x].npages = npages;
    r->ext[x].ceil_w = (joist__slot_c(k, cs, frame_slot)->rights & JOIST_R_WRITE) != 0;
    f->extents++;
    *ext = x;
    return JOIST_OK;
}

joist_err joist_region_map(struct joist_kernel *k, joist_cspace cs, joist_slot region_slot,
                           uint32_t ext, uint64_t page, uint64_t npages, uint64_t va,
                           unsigned perms)
{
    uint32_t rec;
    joist_err e = region_cap(k, cs, region_slot, JOIST_R_MAP, &rec);
    if (e != JOIST_OK) {
        return e;
    }
    const struct joist_region *r = &k->reg[rec];
    if (ext >= r->next) {
        return JOIST_E_RANGE;
    }
    uint32_t fo = r->ext[ext].frame - 1u;
    if (k->obj[fo].dead || k->obj[fo].dying) {
        return JOIST_E_GONE; /* its frame was killed (0022) */
    }
    if (joist__bad_perms(perms) || (perms & ~(unsigned)r->ceil) != 0 ||
        ((perms & JOIST_MAP_W) && !r->ext[ext].ceil_w)) {
        return JOIST_E_RIGHTS;
    }
    if (joist__bad_run(va, npages) || !inside(r, va, npages) || page >= r->ext[ext].npages ||
        npages > r->ext[ext].npages - page) {
        return JOIST_E_RANGE;
    }
    return joist__map_pages(k, r->space - 1u, fo, r->ext[ext].off + page, npages, va, perms,
                            r->ext[ext].ceil_w);
}

joist_err joist_region_unmap(struct joist_kernel *k, joist_cspace cs, joist_slot region_slot,
                             uint64_t va, uint64_t npages)
{
    uint32_t rec;
    joist_err e = region_cap(k, cs, region_slot, JOIST_R_MAP, &rec);
    if (e != JOIST_OK) {
        return e;
    }
    const struct joist_region *r = &k->reg[rec];
    if (joist__bad_run(va, npages) || !inside(r, va, npages)) {
        return JOIST_E_RANGE;
    }
    joist__unmap_pages(k, r->space - 1u, va, npages);
    return JOIST_OK;
}

joist_err joist_region_protect(struct joist_kernel *k, joist_cspace cs, joist_slot region_slot,
                               uint64_t va, uint64_t npages, unsigned perms)
{
    uint32_t rec;
    joist_err e = region_cap(k, cs, region_slot, JOIST_R_MAP, &rec);
    if (e != JOIST_OK) {
        return e;
    }
    const struct joist_region *r = &k->reg[rec];
    if (joist__bad_perms(perms) || (perms & ~(unsigned)r->ceil) != 0) {
        return JOIST_E_RIGHTS;
    }
    if (joist__bad_run(va, npages) || !inside(r, va, npages)) {
        return JOIST_E_RANGE;
    }
    return joist__protect_pages(k, r->space - 1u, va, npages, perms);
}

/* ---- endings ---- */

/* Give back r's extents and take it off its task's list. */
static void let_go(struct joist_kernel *k, uint32_t rec)
{
    struct joist_region *r = &k->reg[rec];
    for (uint32_t x = 0; x < r->next; x++) {
        JOIST_WORK(k);
        uint32_t f = r->ext[x].frame - 1u;
        r->ext[x].frame = 0;
        joist__frame_extent_gone(k, f);
    }
    r->next = 0;
    if (r->space != 0) {
        struct joist_cspace_state *st = &k->cs[r->space - 1u];
        for (uint32_t i = 0; i < JOIST_REGIONS_PER_TASK; i++) {
            JOIST_WORK(k);
            if (st->region[i] == rec + 1u) {
                st->region[i] = 0;
            }
        }
        r->space = 0;
    }
}

/* The record is free once neither its object nor its range is left. */
static void maybe_free(struct joist_kernel *k, uint32_t rec)
{
    struct joist_region *r = &k->reg[rec];
    if (r->obj == 0 && !r->dying && r->space == 0) {
        memset(r, 0, sizeof *r);
    }
}

void joist__region_ends(struct joist_kernel *k, uint32_t obj)
{
    struct joist_object *o = &k->obj[obj];
    uint32_t rec = o->target - 1u;
    struct joist_region *r = &k->reg[rec];
    if (o->refs == 0) {
        r->obj = 0; /* the object goes now (cspace.c frees it) */
    }
    if (r->dying || r->space == 0) {
        maybe_free(k, rec); /* already going, or its task gone already */
        return;
    }
    r->dying = 1;
    r->tsweep = 0;
    k->regions_dying++;
    uint32_t done = 0;
    (void)joist__regions_sweep(k, &done); /* as far as preemption allows */
}

int joist__regions_sweep(struct joist_kernel *k, uint32_t *done)
{
    for (uint32_t rec = 0; k->regions_dying != 0 && rec < JOIST_MAX_REGIONS; rec++) {
        JOIST_WORK(k);
        struct joist_region *r = &k->reg[rec];
        if (!r->live || !r->dying) {
            continue;
        }
        uint32_t s = r->space - 1u;
        /* A task being destroyed sweeps its own tables (task.c); its
         * regions end with it (joist__regions_space_gone). */
        while (r->tsweep < r->tables && joist__usable(k, s)) {
            JOIST_WORK(k);
            if (k->preempt != 0 && *done >= k->preempt) {
                return 1;
            }
            (*done)++;
            joist__unmap_pages(k, s, r->lo + (uint64_t)r->tsweep * JOIST_PT_ENTRIES,
                               JOIST_PT_ENTRIES);
            r->tsweep++;
        }
        if (!joist__usable(k, s)) {
            continue; /* left for the task's end */
        }
        r->dying = 0;
        k->regions_dying--;
        let_go(k, rec);
        maybe_free(k, rec);
    }
    return 0;
}

void joist__regions_space_gone(struct joist_kernel *k, uint32_t s)
{
    struct joist_cspace_state *st = &k->cs[s];
    for (uint32_t i = 0; i < JOIST_REGIONS_PER_TASK; i++) {
        JOIST_WORK(k);
        if (st->region[i] == 0) {
            continue;
        }
        uint32_t rec = st->region[i] - 1u;
        struct joist_region *r = &k->reg[rec];
        if (r->dying) {
            r->dying = 0;
            k->regions_dying--;
        }
        if (r->obj != 0) {
            k->obj[r->obj - 1u].dead = 1; /* its task is gone: E_GONE from now on */
        }
        let_go(k, rec);
        maybe_free(k, rec);
    }
}

/* ---- invariants (I12, extended by 0030) ---- */

unsigned joist__check_region(const struct joist_kernel *k, char *msg, unsigned msglen, int msg_free)
{
    unsigned bad = 0;
#define VIOLATION(...)                                                                             \
    do {                                                                                           \
        JOIST_CBMC_NOTE(__LINE__);                                                                 \
        if (bad++ == 0 && msg_free && msg != NULL && msglen > 0) {                                 \
            JOIST_FMT(msg, msglen, __VA_ARGS__);                                                   \
        }                                                                                          \
    } while (0)

    uint32_t dying = 0;
    for (uint32_t i = 0; i < JOIST_MAX_REGIONS; i++) {
        const struct joist_region *r = &k->reg[i];
        if (!r->live) {
            if (r->obj != 0 || r->space != 0 || r->next != 0 || r->dying) {
                VIOLATION("0030: free region record %u carries state", i);
            }
            continue;
        }
        dying += r->dying;
        if (r->obj == 0 && r->space == 0 && !r->dying) {
            VIOLATION("0030: region %u has neither object nor range, but is kept", i);
        }
        if (r->obj != 0 && (r->obj > JOIST_MAX_OBJECTS || k->obj[r->obj - 1u].type != JOIST_OBJ_REGION ||
                            k->obj[r->obj - 1u].target != i + 1u)) {
            VIOLATION("0030: region %u and its object %u disagree", i, r->obj);
        }
        if (r->lo % JOIST_PT_ENTRIES != 0 || r->tables == 0 || r->tables > JOIST_REGION_TABLES ||
            r->lo >= JOIST_USER_PAGES || span(r) > JOIST_USER_PAGES - r->lo ||
            (r->ceil & ~(unsigned)(JOIST_MAP_W | JOIST_MAP_X)) != 0 || r->next > JOIST_REGION_EXTENTS ||
            r->tsweep > r->tables) {
            VIOLATION("0030: region %u has a bad shape", i);
            continue;
        }
        if (r->dying && r->space == 0) {
            VIOLATION("0030: region %u dying with no task", i);
        }
        if (r->space == 0) {
            if (r->next != 0) {
                VIOLATION("0030: region %u kept extents after its task", i);
            }
            continue;
        }
        uint32_t s = r->space - 1u;
        if (s >= JOIST_MAX_CSPACES || !k->cs[s].live) {
            VIOLATION("0030: region %u names a dead space", i);
            continue;
        }
        uint32_t listed = 0;
        for (uint32_t j = 0; j < JOIST_REGIONS_PER_TASK; j++) {
            listed += k->cs[s].region[j] == i + 1u;
        }
        if (listed != 1) {
            VIOLATION("0030: region %u is listed %u times by its space %u", i, listed, s);
        }
        for (uint32_t x = 0; x < r->next; x++) {
            uint32_t f = r->ext[x].frame;
            if (f == 0 || f > JOIST_MAX_OBJECTS || k->obj[f - 1u].type != JOIST_OBJ_FRAME ||
                r->ext[x].npages == 0 || r->ext[x].off >= k->obj[f - 1u].npages ||
                r->ext[x].npages > k->obj[f - 1u].npages - r->ext[x].off) {
                VIOLATION("0030: region %u extent %u names no window of a frame", i, x);
            }
        }
        /* Every page mapped in the range is a page of one of its extents,
         * within the region's ceiling (and its extent's, for writing). A
         * dying region's tables already swept hold nothing. */
        for (uint64_t p = r->lo; !k->cs[s].dying && p < r->lo + span(r); p++) {
            if (p % JOIST_PT_ENTRIES == 0 && !joist__has_table(k, s, p)) {
                p += JOIST_PT_ENTRIES - 1u; /* no table: nothing mapped there */
                continue;
            }
            {
                uint64_t pe = joist__pte(k, s, p);
                if (pe == 0) {
                    continue;
                }
                uint32_t fo = JOIST_PTE_FRAME(pe);
                int ok = 0;
                for (uint32_t x = 0; x < r->next && !ok; x++) {
                    ok = r->ext[x].frame == fo + 1u && JOIST_PTE_OFF(pe) >= r->ext[x].off &&
                         JOIST_PTE_OFF(pe) - r->ext[x].off < r->ext[x].npages &&
                         ((pe & JOIST_PTE_CEIL_W) == 0 || r->ext[x].ceil_w);
                }
                if (!ok) {
                    VIOLATION("0030: space %u page %llu in region %u is no page of its extents", s,
                              (unsigned long long)p, i);
                }
                if ((JOIST_PTE_PERMS(pe) & ~(unsigned)r->ceil) != 0) {
                    VIOLATION("0030: space %u page %llu is beyond region %u's ceiling", s,
                              (unsigned long long)p, i);
                }
                if (r->dying && (p - r->lo) / JOIST_PT_ENTRIES < r->tsweep) {
                    VIOLATION("0030: region %u's swept table still maps page %llu", i,
                              (unsigned long long)p);
                }
            }
        }
    }
    if (dying != k->regions_dying) {
        VIOLATION("0030: %u regions dying, but regions_dying %u", dying, k->regions_dying);
    }
    /* Each space's list: live records naming it back, ranges apart. */
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        const struct joist_cspace_state *st = &k->cs[s];
        for (uint32_t a = 0; a < JOIST_REGIONS_PER_TASK; a++) {
            uint32_t ra = st->region[a];
            if (ra == 0) {
                continue;
            }
            if (ra > JOIST_MAX_REGIONS || !k->reg[ra - 1u].live || k->reg[ra - 1u].space != s + 1u) {
                VIOLATION("0030: space %u lists region %u, which isn't its", s, ra);
                continue;
            }
            const struct joist_region *x = &k->reg[ra - 1u];
            for (uint32_t b = a + 1u; b < JOIST_REGIONS_PER_TASK; b++) {
                uint32_t rb = st->region[b];
                if (rb == 0 || rb > JOIST_MAX_REGIONS) {
                    continue;
                }
                const struct joist_region *y = &k->reg[rb - 1u];
                if (rb == ra || (x->lo < y->lo + span(y) && y->lo < x->lo + span(x))) {
                    VIOLATION("0030: space %u's regions %u and %u overlap", s, ra, rb);
                }
            }
        }
    }
    /* Each frame's extents count, and each region object's record. */
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *a = &k->obj[o];
        uint32_t n = 0;
        for (uint32_t i = 0; i < JOIST_MAX_REGIONS; i++) {
            for (uint32_t x = 0; k->reg[i].live && x < k->reg[i].next && x < JOIST_REGION_EXTENTS; x++) {
                n += k->reg[i].ext[x].frame == o + 1u;
            }
        }
        if (a->extents != n) {
            VIOLATION("0030: object %u counts %u region extents, but %u name it", o, a->extents, n);
        }
        if (a->type == JOIST_OBJ_REGION &&
            (a->target == 0 || a->target > JOIST_MAX_REGIONS || !k->reg[a->target - 1u].live ||
             k->reg[a->target - 1u].obj != o + 1u)) {
            VIOLATION("0030: region object %u names no record of its own", o);
        }
    }
#undef VIOLATION
    return bad;
}
