/* SPDX-License-Identifier: GPL-3.0-only */
/* cspace.c - capability spaces, objects and capability derivation (Phase 1
 * work item 1; invariants I1, I3, I7, I8).
 *
 * Every operation here touches a fixed number of slots and objects, except
 * joist_cspace_destroy, which is bounded by the space's slots (I9), and
 * joist_cspace_grow, by the leaf pool. */
#include "internal.h"

/* Full rights an object of each type is created with. */
static joist_rights full_rights(joist_obj_type t)
{
    switch (t) {
    case JOIST_OBJ_ENDPOINT:
    case JOIST_OBJ_NOTIFICATION:
        return (joist_rights)(JOIST_R_SEND | JOIST_R_RECV | JOIST_R_GRANT);
    case JOIST_OBJ_FRAME:  /* CONTROL, WRITE (the RECV bit), KILL (0022), GRANT */
    case JOIST_OBJ_REGION: /* CONTROL, MAP (the RECV bit), KILL, GRANT (0030) */
        return (joist_rights)(JOIST_R_SEND | JOIST_R_RECV | JOIST_R_KILL | JOIST_R_GRANT);
    case JOIST_OBJ_TASK:
    case JOIST_OBJ_THREAD:
    case JOIST_OBJ_REPLY:
    case JOIST_OBJ_SCHED:
        /* CONTROL on tasks, threads and scheduling contexts; on a reply
         * object, SEND: may reply.
         * The same bit (JOIST_R_CONTROL is JOIST_R_SEND). */
        return (joist_rights)(JOIST_R_SEND | JOIST_R_GRANT);
    case JOIST_OBJ_NONE:
    default:
        return 0;
    }
}

void joist_init(struct joist_kernel *k)
{
    memset(k, 0, sizeof *k);
}

static joist_err get_space(struct joist_kernel *k, joist_cspace cs, struct joist_cspace_state **out)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    if (!joist__usable(k, cs)) {
        return JOIST_E_DEAD;
    }
    *out = &k->cs[cs];
    return JOIST_OK;
}

joist_err joist_cspace_create(struct joist_kernel *k, uint32_t quota, joist_cspace *out)
{
    if (k->budget > UINT32_MAX || (uint64_t)quota > UINT32_MAX - k->budget) {
        return JOIST_E_NOMEM; /* the budget must fit the quota counters (I3) */
    }
    for (uint32_t i = 0; i < JOIST_MAX_CSPACES; i++) {
        JOIST_WORK(k);
        struct joist_cspace_state *s = &k->cs[i];
        /* A destroyed space is not reused while it is still charged for
         * objects that outlive it, so the charge stays attributable. */
        if (!s->live && s->charged == 0) {
            memset(s, 0, sizeof *s);
            s->live = 1;
            s->quota = quota;
            s->fixture = 1;
            s->mcp = JOIST_PRIO_MAX;
            if (k->hooks.space_new != NULL) {
                k->hooks.space_new(k->hooks.ctx, i);
            }
            k->budget += quota; /* fixture quota enters from nothing (I3) */
            *out = i;
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

/* Drop one reference; destroy the object and refund its donor on the last. */
static void obj_unref(struct joist_kernel *k, uint32_t obj_index)
{
    struct joist_object *o = &k->obj[obj_index];
    o->refs--;
    if (o->refs == 0 && !joist__frame_last_ref(k, obj_index)) {
        joist__obj_free(k, obj_index);
    }
}

void joist__obj_free(struct joist_kernel *k, uint32_t obj_index)
{
    joist__object_freed(k, obj_index);    /* waiters wake; kernel references dropped */
    joist__lifecycle_freed(k, obj_index); /* an unreachable task or thread goes too */
    joist__obj_clear(k, obj_index);
}

void joist__obj_clear(struct joist_kernel *k, uint32_t obj_index)
{
    struct joist_object *o = &k->obj[obj_index];
    {
        struct joist_cspace_state *donor = &k->cs[o->donor];
        donor->charged--;
        if (donor->live) {
            donor->quota++;
        } else {
            k->budget--; /* a dead parentless donor: the unit leaves (I3) */
        }
        o->type = JOIST_OBJ_NONE;
        o->donor = 0;
        o->target = 0;
        o->rseq = 0;
        o->qkind = JOIST_Q_NONE;
        o->qhead = 0;
        o->qtail = 0;
        o->recv_caps = 0;
        o->dead = 0;
        o->ns_notif = 0;
        o->ns_bits = 0;
        o->ns_first = 0;
        o->ns_last = 0;
        o->word = 0;
        o->prio = 0;
        o->budget = 0;
        o->period = 0;
        o->remaining = 0;
        o->refill_at = 0;
        o->base = 0;
        o->npages = 0;
        o->mapcount = 0;
        o->lent = 0;
        o->lent_next = 0;
        o->water = 0;
        o->dying = 0;
        o->extents = 0;
        k->objects_live--;
        k->obj_next[obj_index] = k->obj_free; /* the free list's head (choice 10) */
        k->obj_free = obj_index + 1u;
    }
}

int joist__obj_peek(const struct joist_kernel *k, uint32_t *obj)
{
    if (k->obj_free != 0) {
        *obj = k->obj_free - 1u;
        return 1;
    }
    if (k->obj_high < JOIST_MAX_OBJECTS) {
        *obj = k->obj_high;
        return 1;
    }
    return 0;
}

void joist__obj_take(struct joist_kernel *k, uint32_t obj)
{
    JOIST_WORK(k);
    if (k->obj_free == obj + 1u) {
        k->obj_free = k->obj_next[obj];
        k->obj_next[obj] = 0;
    } else {
        k->obj_high = obj + 1u; /* the first never used */
    }
}

/* ---- the two-level capability table ---- */

const struct joist_cap *joist__leaf_slots_c(const struct joist_kernel *k, uint32_t cs, uint32_t l)
{
    const struct joist_cspace_state *s = &k->cs[cs];
    if (l == 0) {
        return s->slot;
    }
    if (l >= JOIST_CNODE_ROOT || s->leaf[l] == 0 || s->leaf[l] > JOIST_MAX_LEAVES) {
        return NULL;
    }
    return k->leaf[s->leaf[l] - 1u].slot;
}

struct joist_cap *joist__leaf_slots(struct joist_kernel *k, uint32_t cs, uint32_t l)
{
    struct joist_cspace_state *s = &k->cs[cs];
    if (l == 0) {
        return s->slot;
    }
    if (l >= JOIST_CNODE_ROOT || s->leaf[l] == 0 || s->leaf[l] > JOIST_MAX_LEAVES) {
        return NULL;
    }
    return k->leaf[s->leaf[l] - 1u].slot;
}

const struct joist_cap *joist__slot_c(const struct joist_kernel *k, uint32_t cs, joist_slot slot)
{
    if (slot >= JOIST_CSPACE_SLOTS) {
        return NULL;
    }
    const struct joist_cap *leaf = joist__leaf_slots_c(k, cs, slot / JOIST_CNODE_LEAF);
    return leaf != NULL ? &leaf[slot % JOIST_CNODE_LEAF] : NULL;
}

struct joist_cap *joist__slot(struct joist_kernel *k, uint32_t cs, joist_slot slot)
{
    if (slot >= JOIST_CSPACE_SLOTS) {
        return NULL;
    }
    struct joist_cap *leaf = joist__leaf_slots(k, cs, slot / JOIST_CNODE_LEAF);
    return leaf != NULL ? &leaf[slot % JOIST_CNODE_LEAF] : NULL;
}

joist_err joist_cspace_grow(struct joist_kernel *k, joist_cspace cs, uint32_t index)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    if (index == 0 || index >= JOIST_CNODE_ROOT) {
        return JOIST_E_RANGE;
    }
    if (s->leaf[index] != 0) {
        return JOIST_E_OCCUPIED;
    }
    if (s->quota == 0) {
        return JOIST_E_NOMEM;
    }
    for (uint32_t i = 0; i < JOIST_MAX_LEAVES; i++) {
        JOIST_WORK(k);
        struct joist_leaf *l = &k->leaf[i];
        if (l->owner == 0) {
            memset(l, 0, sizeof *l);
            l->owner = cs + 1u;
            l->index = index;
            s->leaf[index] = i + 1u;
            s->quota--;
            s->charged++; /* I3: a leaf is paid for like an object */
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

/* Free leaf `index` of space cs (present and empty) and refund its unit to
 * the space's quota. Used by shrink and by space destruction. */
void joist__leaf_free(struct joist_kernel *k, uint32_t cs, uint32_t index)
{
    struct joist_cspace_state *s = &k->cs[cs];
    struct joist_leaf *l = &k->leaf[s->leaf[index] - 1u];
    l->owner = 0;
    l->index = 0;
    s->leaf[index] = 0;
    s->charged--;
    s->quota++;
}

joist_err joist_cspace_shrink(struct joist_kernel *k, joist_cspace cs, uint32_t index)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    if (index == 0 || index >= JOIST_CNODE_ROOT || s->leaf[index] == 0) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *slots = joist__leaf_slots_c(k, cs, index);
    for (uint32_t i = 0; i < JOIST_CNODE_LEAF; i++) {
        JOIST_WORK(k);
        if (slots[i].obj != 0) {
            return JOIST_E_STATE;
        }
    }
    joist__leaf_free(k, cs, index);
    return JOIST_OK;
}

joist_err joist_obj_create(struct joist_kernel *k, joist_cspace cs, joist_obj_type type,
                           joist_slot dst)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_cap *d = joist__slot(k, cs, dst);
    if (d == NULL) {
        return JOIST_E_RANGE;
    }
    /* Tasks, threads and scheduling contexts have their own constructors. */
    if (full_rights(type) == 0 || type == JOIST_OBJ_TASK || type == JOIST_OBJ_THREAD ||
        type == JOIST_OBJ_SCHED || type == JOIST_OBJ_FRAME || type == JOIST_OBJ_REGION) {
        return JOIST_E_TYPE;
    }
    if (d->obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    if (s->quota == 0) {
        return JOIST_E_NOMEM;
    }
    uint32_t i;
    if (!joist__obj_peek(k, &i)) {
        return JOIST_E_NOMEM;
    }
    joist__obj_take(k, i);
    struct joist_object *o = &k->obj[i];
    /* Start from zero, not from whatever freeing left (CBMC: a reused slot
     * must not carry a reply binding or a queue). */
    memset(o, 0, sizeof *o);
    o->type = (uint8_t)type;
    o->refs = 1;
    o->donor = cs;
    s->quota--;
    s->charged++;
    k->objects_live++;
    *d = (struct joist_cap){.obj = i + 1u, .type = (uint8_t)type, .rights = full_rights(type), .badge = 0};
    joist__cap_added(k, d);
    return JOIST_OK;
}

/* Shared body of derive and mint. */
static joist_err copy_cap(struct joist_kernel *k, joist_cspace cs, joist_slot src, joist_slot dst,
                          joist_rights mask, int set_badge, joist_badge badge)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    (void)s;
    const struct joist_cap *from = joist__slot(k, cs, src);
    struct joist_cap *d = joist__slot(k, cs, dst);
    if (from == NULL || d == NULL) {
        return JOIST_E_RANGE;
    }
    if (from->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (d->obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    if (set_badge) {
        if ((from->rights & JOIST_R_GRANT) == 0) {
            return JOIST_E_RIGHTS;
        }
        if (from->badge != 0 || badge == 0) {
            return JOIST_E_BADGE;
        }
    }
    struct joist_cap to = *from;
    to.rights = (joist_rights)(from->rights & mask); /* I8: a subset, never more */
    if (set_badge) {
        to.badge = badge;
        /* The first send capability for a new badge needs its no-senders
         * account (I4). Allocated here and only here, before anything is
         * changed, so a full table fails the mint cleanly. */
        if (to.type == JOIST_OBJ_ENDPOINT && (to.rights & JOIST_R_SEND) != 0) {
            joist_err r = joist__acct_reserve(k, from->obj - 1u, badge);
            if (r != JOIST_OK) {
                return r;
            }
        }
    }
    k->obj[from->obj - 1u].refs++;
    *d = to;
    joist__cap_added(k, d);
    return JOIST_OK;
}

joist_err joist_cap_derive(struct joist_kernel *k, joist_cspace cs, joist_slot src, joist_slot dst,
                           joist_rights mask)
{
    return copy_cap(k, cs, src, dst, mask, 0, 0);
}

joist_err joist_cap_mint(struct joist_kernel *k, joist_cspace cs, joist_slot src, joist_slot dst,
                         joist_rights mask, joist_badge badge)
{
    return copy_cap(k, cs, src, dst, mask, 1, badge);
}

joist_err joist__cap_grant(struct joist_kernel *k, joist_cspace from, joist_slot src,
                           joist_cspace to, joist_slot dst, joist_rights mask)
{
    struct joist_cspace_state *fs;
    struct joist_cspace_state *ts;
    joist_err e = get_space(k, from, &fs);
    if (e == JOIST_OK) {
        e = get_space(k, to, &ts);
    }
    if (e != JOIST_OK) {
        return e;
    }
    (void)fs;
    (void)ts;
    const struct joist_cap *c = joist__slot(k, from, src);
    struct joist_cap *d = joist__slot(k, to, dst);
    if (c == NULL || d == NULL) {
        return JOIST_E_RANGE;
    }
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if ((c->rights & JOIST_R_GRANT) == 0) {
        return JOIST_E_RIGHTS;
    }
    if (d->obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    /* Same object and badge, so any no-senders account already exists. */
    struct joist_cap copy = *c;
    copy.rights = (joist_rights)(c->rights & mask); /* I8 */
    k->obj[c->obj - 1u].refs++;
    *d = copy;
    joist__cap_added(k, d);
    return JOIST_OK;
}

void joist__cap_delete_slot(struct joist_kernel *k, uint32_t s, joist_slot slot)
{
    struct joist_cap *c = joist__slot(k, s, slot);
    if (c == NULL || c->obj == 0) {
        return; /* callers check first; nothing to delete */
    }
    struct joist_cap gone = *c;
    memset(c, 0, sizeof *c);
    joist__cap_removed(k, &gone); /* may fire no-senders or kill the endpoint */
    obj_unref(k, gone.obj - 1u);  /* may queue an unreachable task */
}

joist_err joist_cap_delete(struct joist_kernel *k, joist_cspace cs, joist_slot slot)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    (void)s;
    const struct joist_cap *c = joist__slot(k, cs, slot);
    if (c == NULL) {
        return JOIST_E_RANGE;
    }
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    joist__cap_delete_slot(k, cs, slot);
    (void)joist__drain_pending(k);
    return JOIST_OK;
}

joist_err joist_cspace_destroy(struct joist_kernel *k, joist_cspace cs)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    if (k->root == cs + 1u) {
        return JOIST_E_STATE; /* the root task never dies (0013) */
    }
    joist__space_destroy(k, cs);
    (void)joist__drain_pending(k);
    return JOIST_OK;
}

/* ---- revoke by kill (decision 0022) ---- */

/* Can objects of type t be killed? Frames (0022) and regions (0030).
 * Endpoints, tasks and threads die their own ways. */
static int killable(joist_obj_type t)
{
    switch (t) {
    case JOIST_OBJ_FRAME:
    case JOIST_OBJ_REGION:
        return 1;
    case JOIST_OBJ_NONE:
    case JOIST_OBJ_ENDPOINT:
    case JOIST_OBJ_NOTIFICATION:
    case JOIST_OBJ_TASK:
    case JOIST_OBJ_THREAD:
    case JOIST_OBJ_REPLY:
    case JOIST_OBJ_SCHED:
    default:
        return 0;
    }
}

joist_err joist_obj_kill(struct joist_kernel *k, joist_cspace cs, joist_slot slot)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    (void)s;
    const struct joist_cap *c = joist__slot_c(k, cs, slot);
    if (c == NULL) {
        return JOIST_E_RANGE;
    }
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (!killable((joist_obj_type)c->type)) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & JOIST_R_KILL) == 0) {
        return JOIST_E_RIGHTS;
    }
    uint32_t obj = c->obj - 1u;
    struct joist_object *o = &k->obj[obj];
    if (o->dead) {
        return JOIST_E_GONE; /* killed already: like every other use */
    }
    if (o->type == JOIST_OBJ_FRAME && o->lent != 0) {
        return JOIST_E_STATE; /* its pages are a space's tables until that space dies (0021) */
    }
    /* Dead, as an endpoint dies (I5): every capability to it now fails with
     * JOIST_E_GONE (joist__control_cap), and the slots stay occupied until
     * their holders delete them, so I1 holds unchanged. The object, and its
     * donor's quota unit, go with the last capability, as for any object. */
    o->dead = 1;
    switch ((joist_obj_type)o->type) {
    case JOIST_OBJ_FRAME:
        /* No new mapping of it (its capabilities fail E_GONE), and the
         * work list's sweep unmaps it everywhere, as a dying frame (0020). */
        joist__frame_killed(k, obj);
        /* Only the frames' sweep, as far as the kernel's preemption allows:
         * a kill begins no space's destruction, so it doesn't take on any
         * already pending (joist_continue does, and the sweep goes round
         * dying spaces too). */
        {
            uint32_t done = 0;
            (void)joist__frames_sweep(k, &done);
        }
        break;
    case JOIST_OBJ_REGION:
        /* Its range unmapped on the work list, its extents given back. */
        joist__region_ends(k, obj);
        break;
    case JOIST_OBJ_NONE:
    case JOIST_OBJ_ENDPOINT:
    case JOIST_OBJ_NOTIFICATION:
    case JOIST_OBJ_TASK:
    case JOIST_OBJ_THREAD:
    case JOIST_OBJ_REPLY:
    case JOIST_OBJ_SCHED:
    default:
        break;
    }
    return JOIST_OK;
}

joist_err joist_cap_read(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                         struct joist_cap *out)
{
    if (cs >= JOIST_MAX_CSPACES || slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    if (!joist__usable(k, cs)) {
        return JOIST_E_DEAD;
    }
    const struct joist_cap *c = joist__slot_c(k, cs, slot);
    if (c == NULL) {
        return JOIST_E_RANGE;
    }
    *out = *c;
    return out->obj != 0 ? JOIST_OK : JOIST_E_EMPTY;
}

unsigned joist_check_invariants(const struct joist_kernel *k, char *msg, unsigned msglen)
{
    unsigned bad = 0;
    uint32_t refs_seen[JOIST_MAX_OBJECTS];
    uint32_t charged_seen[JOIST_MAX_CSPACES];
    uint32_t live = 0;

    memset(refs_seen, 0, sizeof refs_seen);
    memset(charged_seen, 0, sizeof charged_seen);

#define VIOLATION(...)                                                                             \
    do {                                                                                           \
        JOIST_CBMC_NOTE(__LINE__);                                                                 \
        if (bad++ == 0 && msg != NULL && msglen > 0) {                                             \
            JOIST_FMT(msg, msglen, __VA_ARGS__);                                              \
        }                                                                                          \
    } while (0)

    /* The table's own structure: every pool leaf in use is named by exactly
     * the space and index it records, and only live spaces have leaves. */
    for (uint32_t i = 0; i < JOIST_MAX_LEAVES; i++) {
        const struct joist_leaf *l = &k->leaf[i];
        if (l->owner == 0) {
            for (uint32_t j = 0; j < JOIST_CNODE_LEAF; j++) {
                if (l->slot[j].obj != 0) {
                    VIOLATION("I1: free leaf %u holds a capability in slot %u", i, j);
                    break;
                }
            }
            continue;
        }
        if (l->owner > JOIST_MAX_CSPACES || l->index == 0 || l->index >= JOIST_CNODE_ROOT) {
            VIOLATION("I3: leaf %u owner %u index %u out of range", i, l->owner, l->index);
            continue;
        }
        if (k->cs[l->owner - 1u].leaf[l->index] != i + 1u) {
            VIOLATION("I3: leaf %u claims space %u index %u, which names %u", i, l->owner - 1u,
                      l->index, k->cs[l->owner - 1u].leaf[l->index]);
        }
        if (!k->cs[l->owner - 1u].live) {
            VIOLATION("I3: dead space %u still has leaf %u", l->owner - 1u, i);
        }
        charged_seen[l->owner - 1u]++;
    }
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        const struct joist_cspace_state *s = &k->cs[c];
        if (s->leaf[0] != 0) {
            VIOLATION("I3: space %u root entry 0 is %u, not the built-in leaf", c, s->leaf[0]);
        }
        for (uint32_t l = 1; l < JOIST_CNODE_ROOT; l++) {
            uint32_t n = s->leaf[l];
            if (n != 0 && (n > JOIST_MAX_LEAVES || k->leaf[n - 1u].owner != c + 1u ||
                           k->leaf[n - 1u].index != l)) {
                VIOLATION("I3: space %u root entry %u names leaf %u, not its own", c, l, n);
            }
        }
    }
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        const struct joist_cspace_state *s = &k->cs[c];
        for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
            if (i % JOIST_CNODE_LEAF == 0 && joist__leaf_slots_c(k, c, i / JOIST_CNODE_LEAF) == NULL) {
                i += JOIST_CNODE_LEAF - 1u; /* absent leaf: skip it */
                continue;
            }
            const struct joist_cap *cap = joist__slot_c(k, c, i);
            if (cap == NULL || cap->obj == 0) {
                continue;
            }
            if (!s->live) {
                VIOLATION("I1: dead space %u still holds a capability in slot %u", c, i);
                continue;
            }
            if (cap->obj > JOIST_MAX_OBJECTS) {
                VIOLATION("I1: space %u slot %u names object %u out of range", c, i, cap->obj);
                continue;
            }
            const struct joist_object *o = &k->obj[cap->obj - 1u];
            if (o->type == JOIST_OBJ_NONE) {
                VIOLATION("I1: space %u slot %u names freed object %u", c, i, cap->obj - 1u);
                continue;
            }
            if (cap->type != o->type) {
                VIOLATION("I1: space %u slot %u type %u != object type %u", c, i, cap->type,
                          o->type);
            }
            if ((cap->rights & (joist_rights)~full_rights((joist_obj_type)o->type)) != 0) {
                VIOLATION("I8: space %u slot %u has rights 0x%x beyond the type's 0x%x", c, i,
                          cap->rights, full_rights((joist_obj_type)o->type));
            }
            refs_seen[cap->obj - 1u]++;
        }
    }
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        const struct joist_object *o = &k->obj[i];
        if (o->type == JOIST_OBJ_NONE) {
            if (o->refs != 0) {
                VIOLATION("I1: freed object %u has refs %u", i, o->refs);
            }
            continue;
        }
        live++;
        if (o->refs != refs_seen[i]) {
            VIOLATION("I1: object %u refs %u but %u capabilities name it", i, o->refs,
                      refs_seen[i]);
        }
        if (o->refs == 0 && !(o->type == JOIST_OBJ_FRAME && o->dying)) {
            /* (A dying frame is kept by its mappings or its lending: I12.) */
            VIOLATION("I1: live object %u has no capabilities (leaked)", i);
        }
        if (o->donor >= JOIST_MAX_CSPACES) {
            VIOLATION("I3: object %u donor %u out of range", i, o->donor);
        } else {
            charged_seen[o->donor]++;
        }
    }
    if (live != k->objects_live) {
        VIOLATION("I3: objects_live %u but %u objects are live", k->objects_live, live);
    }
    /* The free objects (choice 10): none at or above obj_high was ever
     * used, and the list from obj_free names each free one below it once,
     * ending; no other object has a link. */
    static uint8_t listed[JOIST_MAX_OBJECTS];
    if (k->obj_high > JOIST_MAX_OBJECTS) {
        VIOLATION("free objects: high-water mark %u past the table", k->obj_high);
    } else {
        uint32_t nfree = 0;
        for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
            listed[i] = 0;
            if (i >= k->obj_high && k->obj[i].type != JOIST_OBJ_NONE) {
                VIOLATION("free objects: object %u in use above the high-water mark %u", i,
                          k->obj_high);
            }
            nfree += (uint32_t)(i < k->obj_high && k->obj[i].type == JOIST_OBJ_NONE);
        }
        uint32_t onlist = 0;
        uint32_t p = k->obj_free;
        int sound = 1;
        for (uint32_t n = 0; p != 0 && n < JOIST_MAX_OBJECTS; n++) {
            if (p > k->obj_high || k->obj[p - 1u].type != JOIST_OBJ_NONE || listed[p - 1u]) {
                VIOLATION("free objects: the list names %u, in use, unused or twice", p - 1u);
                sound = 0;
                break;
            }
            listed[p - 1u] = 1;
            onlist++;
            p = k->obj_next[p - 1u];
        }
        if (sound && p != 0) {
            VIOLATION("free objects: the list does not end");
        } else if (sound && onlist != nfree) {
            VIOLATION("free objects: %u listed of %u free below the mark", onlist, nfree);
        }
        for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
            if (!listed[i] && k->obj_next[i] != 0) {
                VIOLATION("free objects: object %u off the list has a link", i);
            }
        }
    }
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        if (k->cs[c].charged != charged_seen[c]) {
            VIOLATION("I3: space %u charged %u but pays for %u live objects and leaves", c,
                      k->cs[c].charged, charged_seen[c]);
        }
    }
#undef VIOLATION
    unsigned ipc = joist__check_ipc(k, msg, msglen, bad == 0);
    unsigned note = joist__check_notify(k, msg, msglen, bad + ipc == 0);
    unsigned task = joist__check_task(k, msg, msglen, bad + ipc + note == 0);
    unsigned sched = joist__check_sched(k, msg, msglen, bad + ipc + note + task == 0);
    unsigned map = joist__check_map(k, msg, msglen, bad + ipc + note + task + sched == 0);
    return bad + ipc + note + task + sched + map +
           joist__check_region(k, msg, msglen, bad + ipc + note + task + sched + map == 0);
}

#if !__STDC_HOSTED__
/* Freestanding JOIST_FMT: copy the format text itself, bounded. */
void joist__fmt_bare(char *msg, unsigned msglen, const char *fmt)
{
    if (msglen == 0) {
        return;
    }
    unsigned i = 0;
    for (; i + 1u < msglen && fmt[i] != '\0'; i++) {
        msg[i] = fmt[i];
    }
    msg[i] = '\0';
}
#endif

const char *joist_err_name(joist_err e)
{
    switch (e) {
    case JOIST_OK:
        return "OK";
    case JOIST_E_RANGE:
        return "E_RANGE";
    case JOIST_E_EMPTY:
        return "E_EMPTY";
    case JOIST_E_OCCUPIED:
        return "E_OCCUPIED";
    case JOIST_E_RIGHTS:
        return "E_RIGHTS";
    case JOIST_E_NOMEM:
        return "E_NOMEM";
    case JOIST_E_BADGE:
        return "E_BADGE";
    case JOIST_E_TYPE:
        return "E_TYPE";
    case JOIST_E_DEAD:
        return "E_DEAD";
    case JOIST_E_TIMEOUT:
        return "E_TIMEOUT";
    case JOIST_E_GONE:
        return "E_GONE";
    case JOIST_E_PEER:
        return "E_PEER";
    case JOIST_E_NOREPLY:
        return "E_NOREPLY";
    case JOIST_E_STATE:
        return "E_STATE";
    case JOIST_E_ABORTED:
        return "E_ABORTED";
    case JOIST_BLOCKED:
        return "BLOCKED";
    default:
        return "E_?";
    }
}
