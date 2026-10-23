/* map.c - frames and their mappings (Phase 3 step 3; decision 0013, plan
 * §5.5 and docs/phase3-plan.md choice 5; invariant I12).
 *
 * A frame object names a range of physical pages. The kernel makes frames
 * for free memory at boot and gives them to the root task (0013), which
 * splits them and maps them into tasks; frames never overlap, so no page is
 * handed out twice. A mapping places a whole frame at a range of a task's
 * user half; the task pays one quota unit for it, standing for its page
 * tables. Deleting a frame's last capability unmaps it everywhere, and a
 * dying task's mappings go with it. The kernel's page-table code
 * (kernel/mem/vm.c) carries the mappings out; this file decides them.
 * Every operation is bounded by JOIST_MAX_MAPPINGS or JOIST_MAX_OBJECTS. */
#include "internal.h"

/* A new object of `type` in slot d of space cs, from cs's quota: the shared
 * end of boot_frame and frame_split. */
static joist_err new_frame(struct joist_kernel *k, uint32_t cs, struct joist_cap *d, uint64_t base,
                           uint64_t npages, joist_rights rights)
{
    struct joist_cspace_state *s = &k->cs[cs];
    if (s->quota == 0) {
        return JOIST_E_NOMEM;
    }
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        JOIST_WORK(k);
        struct joist_object *o = &k->obj[i];
        if (o->type == JOIST_OBJ_NONE) {
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
    }
    return JOIST_E_NOMEM;
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
    if (npages == 0 || base > UINT64_MAX - npages) {
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
    /* Only an unshared, unmapped frame: otherwise someone's view of it, or a
     * mapping of it, would change under them. */
    if (f->refs != 1) {
        return JOIST_E_STATE;
    }
    for (uint32_t i = 0; i < JOIST_MAX_MAPPINGS; i++) {
        JOIST_WORK(k);
        if (k->map[i].space != 0 && k->map[i].frame == fo + 1u) {
            return JOIST_E_STATE;
        }
    }
    joist_rights rights = joist__slot_c(k, cs, slot)->rights;
    uint64_t rest = f->npages - at;
    e = new_frame(k, cs, d, f->base + at, rest, rights);
    if (e == JOIST_OK) {
        f->npages = at;
    }
    return e;
}

joist_err joist_map(struct joist_kernel *k, joist_cspace cs, joist_slot frame_slot,
                    joist_slot task_slot, uint64_t va, unsigned perms)
{
    uint32_t fo;
    uint32_t to = 0;
    joist_err e = joist__control_cap(k, cs, frame_slot, JOIST_OBJ_FRAME, &fo);
    if (e == JOIST_OK && task_slot != JOIST_SLOT_SELF) {
        e = joist__control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &to);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (perms & ~(unsigned)(JOIST_MAP_W | JOIST_MAP_X) ||
        ((perms & JOIST_MAP_W) && (perms & JOIST_MAP_X))) {
        return JOIST_E_RIGHTS; /* W^X, always */
    }
    if ((perms & JOIST_MAP_W) && (joist__slot_c(k, cs, frame_slot)->rights & JOIST_R_WRITE) == 0) {
        return JOIST_E_RIGHTS;
    }
    const struct joist_object *f = &k->obj[fo];
    if (va >= JOIST_USER_PAGES || f->npages > JOIST_USER_PAGES - va) {
        return JOIST_E_RANGE; /* the user half only */
    }
    if (f->npages > JOIST_MAP_SPAN - va % JOIST_MAP_SPAN) {
        return JOIST_E_RANGE; /* within one page table (JOIST_MAP_SPAN) */
    }
    uint32_t space = task_slot == JOIST_SLOT_SELF ? cs : k->obj[to].target - 1u;
    uint32_t freeslot = JOIST_MAX_MAPPINGS;
    for (uint32_t i = 0; i < JOIST_MAX_MAPPINGS; i++) {
        JOIST_WORK(k);
        const struct joist_mapping *m = &k->map[i];
        if (m->space == 0) {
            if (freeslot == JOIST_MAX_MAPPINGS) {
                freeslot = i;
            }
        } else if (m->space == space + 1u && va < m->va + m->npages && m->va < va + f->npages) {
            return JOIST_E_OCCUPIED;
        }
    }
    struct joist_cspace_state *ts = &k->cs[space];
    if (ts->quota == 0 || freeslot == JOIST_MAX_MAPPINGS) {
        return JOIST_E_NOMEM;
    }
    k->map[freeslot] = (struct joist_mapping){
        .space = space + 1u, .frame = fo + 1u, .va = va, .npages = f->npages, .perms = (uint8_t)perms};
    ts->quota--;
    ts->charged++; /* the task pays for its page tables (I3) */
    if (k->hooks.map != NULL) {
        k->hooks.map(k->hooks.ctx, space, va, f->base, f->npages, perms);
    }
    return JOIST_OK;
}

/* Free mapping i, refunding its space. */
static void unmap_one(struct joist_kernel *k, uint32_t i)
{
    if (k->hooks.unmap != NULL) {
        k->hooks.unmap(k->hooks.ctx, k->map[i].space - 1u, k->map[i].va, k->map[i].npages);
    }
    struct joist_cspace_state *s = &k->cs[k->map[i].space - 1u];
    s->charged--;
    s->quota++;
    memset(&k->map[i], 0, sizeof k->map[i]);
}

joist_err joist_unmap(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot, uint64_t va)
{
    uint32_t to = 0;
    joist_err e;
    if (task_slot != JOIST_SLOT_SELF) {
        e = joist__control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &to);
    } else if (cs >= JOIST_MAX_CSPACES) {
        e = JOIST_E_RANGE;
    } else {
        e = joist__usable(k, cs) ? JOIST_OK : JOIST_E_DEAD;
    }
    if (e != JOIST_OK) {
        return e;
    }
    uint32_t space = task_slot == JOIST_SLOT_SELF ? cs : k->obj[to].target - 1u;
    for (uint32_t i = 0; i < JOIST_MAX_MAPPINGS; i++) {
        JOIST_WORK(k);
        if (k->map[i].space == space + 1u && k->map[i].va == va) {
            unmap_one(k, i);
            return JOIST_OK;
        }
    }
    return JOIST_E_EMPTY;
}

void joist__frame_freed(struct joist_kernel *k, uint32_t obj)
{
    for (uint32_t i = 0; i < JOIST_MAX_MAPPINGS; i++) {
        JOIST_WORK(k);
        if (k->map[i].space != 0 && k->map[i].frame == obj + 1u) {
            unmap_one(k, i);
        }
    }
}

void joist__space_unmap_all(struct joist_kernel *k, uint32_t cs)
{
    for (uint32_t i = 0; i < JOIST_MAX_MAPPINGS; i++) {
        JOIST_WORK(k);
        if (k->map[i].space == cs + 1u) {
            unmap_one(k, i);
        }
    }
}

/* ---- invariants (I12) ---- */

unsigned joist__check_map(const struct joist_kernel *k, char *msg, unsigned msglen, int msg_free)
{
    unsigned bad = 0;
#define VIOLATION(...)                                                                             \
    do {                                                                                           \
        if (bad++ == 0 && msg_free && msg != NULL && msglen > 0) {                                 \
            JOIST_FMT(msg, msglen, __VA_ARGS__);                                                   \
        }                                                                                          \
    } while (0)

    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        const struct joist_object *a = &k->obj[i];
        if (a->type != JOIST_OBJ_FRAME) {
            continue;
        }
        if (a->npages == 0 || a->base > UINT64_MAX - a->npages) {
            VIOLATION("I12: frame %u is empty or wraps", i);
            continue;
        }
        for (uint32_t j = i + 1u; j < JOIST_MAX_OBJECTS; j++) {
            const struct joist_object *b = &k->obj[j];
            if (b->type == JOIST_OBJ_FRAME && a->base < b->base + b->npages &&
                b->base < a->base + a->npages) {
                VIOLATION("I12: frames %u and %u overlap", i, j);
            }
        }
    }
    for (uint32_t i = 0; i < JOIST_MAX_MAPPINGS; i++) {
        const struct joist_mapping *m = &k->map[i];
        if (m->space == 0) {
            if (m->frame != 0 || m->va != 0 || m->npages != 0 || m->perms != 0) {
                VIOLATION("I12: free mapping %u carries state", i);
            }
            continue;
        }
        if (m->space > JOIST_MAX_CSPACES || !k->cs[m->space - 1u].live) {
            VIOLATION("I12: mapping %u in dead or invalid space %u", i, m->space - 1u);
        }
        if (m->frame == 0 || m->frame > JOIST_MAX_OBJECTS ||
            k->obj[m->frame - 1u].type != JOIST_OBJ_FRAME ||
            k->obj[m->frame - 1u].npages != m->npages) {
            VIOLATION("I12: mapping %u doesn't map a whole live frame", i);
        }
        if (m->va >= JOIST_USER_PAGES || m->npages > JOIST_USER_PAGES - m->va) {
            VIOLATION("I12: mapping %u outside the user half", i);
        }
        if ((m->perms & JOIST_MAP_W) && (m->perms & JOIST_MAP_X)) {
            VIOLATION("I12: mapping %u is writable and executable", i);
        }
        for (uint32_t j = i + 1u; j < JOIST_MAX_MAPPINGS; j++) {
            const struct joist_mapping *n = &k->map[j];
            if (n->space == m->space && m->va < n->va + n->npages && n->va < m->va + m->npages) {
                VIOLATION("I12: mappings %u and %u overlap in space %u", i, j, m->space - 1u);
            }
        }
    }
#undef VIOLATION
    return bad;
}
