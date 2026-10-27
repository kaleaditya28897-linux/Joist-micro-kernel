/* task.c - tasks and threads as capability-named objects, boot, and task
 * destruction (Phase 1 work item 4; invariants I1, I3; decisions 0004, 0010,
 * 0013, 0016).
 *
 * Destruction cascades: destroying a task deletes its capabilities, and if
 * one was the last capability to another live task, that task becomes
 * unreachable and must go too. It is done in three parts (decision 0016,
 * Phase 3 step 3), checked against the order the TLA+ model verified
 * (model/tla/IpcDelete.tla):
 *
 *  1. begin (joist__space_destroy), bounded: the space becomes *dying*, so
 *     every operation treats it as dead and nothing can put a capability
 *     into it (TLA+'s rule); its task object dies; its threads are
 *     destroyed; it goes on the work list (k->pending).
 *  2. sweep, one slot per unit of work, from the space's own cursor: the
 *     preemptible part. A deletion that makes another task unreachable
 *     begins that task's destruction at once and pushes it on top, so the
 *     newest is swept first and the older one resumes at its cursor later.
 *     No recursion: a loop over the work list.
 *  3. finish, bounded: free its leaves and mappings, hand its children,
 *     quota and charges to its parent, and mark it dead.
 *
 * With k->preempt nonzero, a sweep stops after that many slots and the
 * operation returns with the rest pending, for joist_continue. */
#include "internal.h"

/* ---- the destruction work list ---- */

static void pending_push(struct joist_kernel *k, uint32_t s)
{
    for (uint32_t i = 0; i < k->npending; i++) {
        JOIST_WORK(k);
        if (k->pending[i] == s) {
            return;
        }
    }
    k->pending[k->npending++] = s;
}

void joist__space_destroy(struct joist_kernel *k, uint32_t s)
{
    struct joist_cspace_state *st = &k->cs[s];
    if (!st->live || st->dying) {
        return; /* already gone, or already going */
    }
    /* Dying first, so nothing is put into it from here on, and deleting this
     * task's own last capability during the sweep doesn't begin it again. */
    st->dying = 1;
    st->sweep = 0;
    if (st->task_obj != 0) {
        k->obj[st->task_obj - 1u].dead = 1;
        st->task_obj = 0;
    }
    /* Threads before capabilities: they may hold reply capabilities whose
     * callers must wake, and with them gone nothing in the space can act. */
    joist__threads_destroy_owned(k, s);
    pending_push(k, s);
}

/* Part 3: everything a fully swept space still owns goes to its parent. */
static void space_finish(struct joist_kernel *k, uint32_t s)
{
    struct joist_cspace_state *st = &k->cs[s];
    for (uint32_t l = 1; l < JOIST_CNODE_ROOT; l++) {
        JOIST_WORK(k);
        if (st->leaf[l] != 0) {
            joist__leaf_free(k, s, l); /* empty: swept */
        }
    }
    if (k->hooks.space_gone != NULL) {
        k->hooks.space_gone(k->hooks.ctx, s); /* its hardware tables, all at once */
    }
    joist__space_tables_gone(k, s); /* the frames lent for them are their owners' again */
    /* I3: hand everything this space still owns to its parent, and its
     * children too, so no quota is lost and the parent chain stays live. */
    uint32_t p = st->parent;
    /* Children go to the grandparent. A parentless space (only test
     * fixtures; every real task descends from the root) hands them to the
     * root, so no task is ever left without an ancestor to reap it. */
    uint32_t heir = p != 0 ? p : k->root;
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        JOIST_WORK(k);
        if (k->cs[c].live && k->cs[c].parent == s + 1u) {
            k->cs[c].parent = heir;
        }
    }
    if (p != 0) {
        struct joist_cspace_state *ps = &k->cs[p - 1u];
        ps->quota += st->quota;
        for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
            JOIST_WORK(k);
            if (k->obj[o].type != JOIST_OBJ_NONE && k->obj[o].donor == s) {
                k->obj[o].donor = p - 1u;
                ps->charged++;
                st->charged--;
            }
        }
    } else {
        k->budget -= st->quota; /* a parentless (fixture) space's quota leaves */
    }
    st->quota = 0;
    st->parent = 0;
    st->fixture = 0;
    st->dying = 0;
    st->sweep = 0;
    st->live = 0;
}

int joist__drain_pending(struct joist_kernel *k)
{
    uint32_t done = 0;
    while (k->npending != 0) {
        JOIST_WORK(k);
        uint32_t s = k->pending[k->npending - 1u];
        struct joist_cspace_state *st = &k->cs[s];
        if (st->sweep < JOIST_CSPACE_SLOTS) {
            if (k->preempt != 0 && done >= k->preempt) {
                return 1; /* a preemption point: the rest waits for joist_continue */
            }
            done++;
            joist_slot slot = st->sweep;
            if (slot % JOIST_CNODE_LEAF == 0 && joist__leaf_slots_c(k, s, slot / JOIST_CNODE_LEAF) == NULL) {
                st->sweep += JOIST_CNODE_LEAF; /* an absent leaf: nothing to sweep */
                continue;
            }
            st->sweep++;
            if (joist__slot_c(k, s, slot)->obj != 0) {
                joist__cap_delete_slot(k, s, slot); /* may begin unreachable tasks */
            }
            continue;
        }
        /* Then its page tables (0020), a last-level table per step. */
        if (joist__space_tables_sweep(k, s, &done)) {
            return 1;
        }
        /* Fully swept, and on top: nothing pushed since is left. */
        k->npending--;
        space_finish(k, s);
    }
    /* Frames that died while mapped (0020): unmapped everywhere, a table per
     * step, after the spaces (whose own sweeps unmap their share). */
    return joist__frames_sweep(k, &done);
}

joist_err joist_continue(struct joist_kernel *k, uint32_t budget)
{
    uint32_t saved = k->preempt;
    k->preempt = budget;
    int more = joist__drain_pending(k);
    k->preempt = saved;
    return more ? JOIST_BLOCKED : JOIST_OK;
}

/* The last capability to a task or thread object is gone. */
void joist__lifecycle_freed(struct joist_kernel *k, uint32_t obj)
{
    struct joist_object *o = &k->obj[obj];
    if (o->type == JOIST_OBJ_TASK && !o->dead && o->target == k->root) {
        /* The root's own capability to itself (joist_boot_self): the root is
         * never destroyed (0013); it just has none any more. */
        k->cs[o->target - 1u].task_obj = 0;
        return;
    }
    if (o->type == JOIST_OBJ_TASK && !o->dead && o->target != 0) {
        /* Unreachable now: its destruction begins at once (dying, threads
         * gone, on the work list), so no window is left in which a doomed
         * task could still receive a capability. Beginning clears the
         * space's back-pointer to this object, which is about to be freed. */
        joist__space_destroy(k, o->target - 1u);
    }
    if (o->type == JOIST_OBJ_THREAD && !o->dead && o->target != 0) {
        uint32_t t = o->target - 1u;
        k->th[t].obj = 0; /* the object is going away; don't mark it */
        (void)joist_thread_destroy(k, t);
    }
}

/* ---- boot and tasks ---- */

joist_err joist_boot(struct joist_kernel *k, uint32_t budget, joist_cspace *root)
{
    if (k->budget != 0 || k->root != 0 || k->cs[0].live) {
        return JOIST_E_STATE;
    }
    struct joist_cspace_state *s = &k->cs[0];
    memset(s, 0, sizeof *s);
    s->live = 1;
    s->quota = budget;
    s->mcp = JOIST_PRIO_MAX; /* the root may give any priority (0013) */
    k->budget = budget;
    k->root = 1;
    *root = 0;
    if (k->hooks.space_new != NULL) {
        k->hooks.space_new(k->hooks.ctx, 0);
    }
    return JOIST_OK;
}

static joist_err space_live(const struct joist_kernel *k, joist_cspace cs)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    return joist__usable(k, cs) ? JOIST_OK : JOIST_E_DEAD;
}

/* A free object, not yet taken: install takes it. */
static int free_object(const struct joist_kernel *k, uint32_t *out)
{
    return joist__obj_peek(k, out);
}

/* Install a new object and its capability (all checks already done). */
static void install(struct joist_kernel *k, joist_cspace cs, joist_slot dst, uint32_t obj,
                    joist_obj_type type, uint32_t target)
{
    joist__obj_take(k, obj);
    struct joist_object *o = &k->obj[obj];
    memset(o, 0, sizeof *o);
    o->type = (uint8_t)type;
    o->refs = 1;
    o->donor = cs;
    o->target = target;
    k->cs[cs].quota--;
    k->cs[cs].charged++;
    k->objects_live++;
    struct joist_cap *d = joist__slot(k, cs, dst); /* checked by the callers */
    *d = (struct joist_cap){.obj = obj + 1u,
                            .type = (uint8_t)type,
                            .rights = (joist_rights)(JOIST_R_CONTROL | JOIST_R_GRANT),
                            .badge = 0};
    joist__cap_added(k, d);
}

joist_err joist__task_cap_new(struct joist_kernel *k, joist_cspace cs, joist_slot dst,
                              uint32_t space)
{
    uint32_t obj;
    const struct joist_cap *d = joist__slot_c(k, cs, dst);
    if (d == NULL) {
        return JOIST_E_RANGE;
    }
    if (d->obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    if (k->cs[cs].quota == 0 || !free_object(k, &obj)) {
        return JOIST_E_NOMEM;
    }
    install(k, cs, dst, obj, JOIST_OBJ_TASK, space + 1u);
    k->cs[space].task_obj = obj + 1u;
    return JOIST_OK;
}

joist_err joist_task_create(struct joist_kernel *k, joist_cspace cs, uint32_t donation,
                            joist_slot dst)
{
    uint32_t obj;
    uint32_t ns = JOIST_MAX_CSPACES;
    joist_err e = space_live(k, cs);
    if (e != JOIST_OK) {
        return e;
    }
    const struct joist_cap *d = joist__slot_c(k, cs, dst);
    if (d == NULL) {
        return JOIST_E_RANGE;
    }
    if (d->obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    /* The creator pays the donation plus one for the task object (0004). */
    if (donation == UINT32_MAX || k->cs[cs].quota < donation + 1u) {
        return JOIST_E_NOMEM;
    }
    for (uint32_t i = 0; i < JOIST_MAX_CSPACES; i++) {
        JOIST_WORK(k);
        if (!k->cs[i].live && k->cs[i].charged == 0) {
            ns = i;
            break;
        }
    }
    if (ns == JOIST_MAX_CSPACES || !free_object(k, &obj)) {
        return JOIST_E_NOMEM;
    }
    install(k, cs, dst, obj, JOIST_OBJ_TASK, ns + 1u);
    struct joist_cspace_state *n = &k->cs[ns];
    memset(n, 0, sizeof *n);
    n->live = 1;
    n->quota = donation;
    n->parent = cs + 1u;
    n->mcp = k->cs[cs].mcp; /* lowered with joist_task_set_mcp */
    n->task_obj = obj + 1u;
    k->cs[cs].quota -= donation;
    if (k->hooks.space_new != NULL) {
        k->hooks.space_new(k->hooks.ctx, ns); /* its page tables (kernel) */
    }
    return JOIST_OK;
}

/* Look up a live task or thread capability with CONTROL. */
static joist_err control_cap(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                             joist_obj_type type, uint32_t *obj)
{
    const struct joist_cap *c = joist__slot_c(k, cs, slot);
    if (c == NULL) {
        return JOIST_E_RANGE;
    }
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (c->type != type) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & JOIST_R_CONTROL) == 0) {
        return JOIST_E_RIGHTS;
    }
    if (k->obj[c->obj - 1u].dead) {
        return JOIST_E_GONE;
    }
    *obj = c->obj - 1u;
    return JOIST_OK;
}

joist_err joist_task_destroy(struct joist_kernel *k, joist_cspace cs, joist_slot slot)
{
    uint32_t obj;
    joist_err e = space_live(k, cs);
    if (e == JOIST_OK) {
        e = control_cap(k, cs, slot, JOIST_OBJ_TASK, &obj);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (k->obj[obj].target == k->root) {
        return JOIST_E_RIGHTS; /* the root's own capability (joist_boot_self): never (0013) */
    }
    joist__space_destroy(k, k->obj[obj].target - 1u);
    (void)joist__drain_pending(k);
    return JOIST_OK;
}

joist_err joist_cap_give(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                         joist_slot src, joist_slot dst, joist_rights mask)
{
    uint32_t obj;
    joist_err e = space_live(k, cs);
    if (e == JOIST_OK) {
        e = control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &obj);
    }
    if (e != JOIST_OK) {
        return e;
    }
    /* The copy itself, with its checks and accounting: the target is alive
     * (its task capability isn't dead), so not dying either. */
    return joist__cap_grant(k, cs, src, k->obj[obj].target - 1u, dst, mask);
}

joist_err joist_cap_copy_batch(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                               joist_slot first, uint32_t count, joist_rights mask,
                               uint32_t flags, uint32_t *done, uint32_t *skipped)
{
    uint32_t obj;
    *done = 0;
    *skipped = 0;
    joist_err e = space_live(k, cs);
    if (e == JOIST_OK) {
        e = control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &obj); /* CONTROL on the child */
    }
    if (e == JOIST_E_GONE) { /* the child is dying or destroyed */
        /* Its task object dies as its destruction begins (0016), and a
         * dying space accepts no capability (TLA+'s rule,
         * model/tla/IpcDelete.tla). So a live task object names a space
         * that is live and not dying. */
        return JOIST_E_DEAD;
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (count > JOIST_COPY_BATCH || first >= JOIST_CSPACE_SLOTS ||
        count > JOIST_CSPACE_SLOTS - first || (flags & ~(uint32_t)JOIST_COPY_RECV) != 0) {
        return JOIST_E_RANGE;
    }
    uint32_t child = k->obj[obj].target - 1u;
    for (uint32_t i = 0; i < count; i++) {
        JOIST_WORK(k);
        joist_slot s = first + i;
        const struct joist_cap *c = joist__slot_c(k, cs, s);
        if (c == NULL || c->obj == 0) {
            continue; /* nothing here (an absent leaf of the caller's holds nothing) */
        }
        /* Mach's fork inherits no receive right; glibc makes new ones. */
        int recv = c->type == JOIST_OBJ_ENDPOINT && (c->rights & mask & JOIST_R_RECV) != 0 &&
                   (flags & JOIST_COPY_RECV) == 0;
        if ((c->rights & JOIST_R_GRANT) == 0 || recv) {
            *skipped |= 1u << i;
            continue;
        }
        const struct joist_cap *dc = joist__slot_c(k, child, s);
        if (dc == NULL) {
            *done = i; /* the caller grows the child's table, and goes on from here */
            return JOIST_E_RANGE;
        }
        if (dc->obj != 0) {
            *done = i; /* what was copied before stays copied */
            return JOIST_E_OCCUPIED;
        }
        /* The copy, with cap_give's accounting: one more reference, RECV
         * counted, and the badge's no-senders account (which exists: c is
         * counted in it) one more sender. Nothing is allocated, nothing
         * fires. Checked above, so it can't fail. */
        e = joist__cap_grant(k, cs, s, child, s, mask);
        if (e != JOIST_OK) {
            *done = i;
            return e;
        }
    }
    *done = count;
    return JOIST_OK;
}

/* Is space s a strict descendant of space anc (anc on s's parent chain)?
 * Bounded by JOIST_MAX_CSPACES: parent chains are acyclic (an invariant),
 * and the walk stops after that many steps regardless. */
static int is_descendant(struct joist_kernel *k, uint32_t anc, uint32_t s)
{
    uint32_t p = k->cs[s].parent;
    for (uint32_t steps = 0; p != 0 && steps < JOIST_MAX_CSPACES; steps++) {
        JOIST_WORK(k);
        if (p - 1u == anc) {
            return 1;
        }
        p = k->cs[p - 1u].parent;
    }
    return 0;
}

joist_err joist_task_reap(struct joist_kernel *k, joist_cspace cs, joist_cspace target)
{
    joist_err e = space_live(k, cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (target >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    if (!joist__usable(k, target)) { /* dead, or already dying */
        return JOIST_E_DEAD;
    }
    /* The authority is ancestry: the parent chain records who donated the
     * task's memory (0004 amendment). The root and fixtures have no parent,
     * so nobody reaps them, and nobody reaps itself. */
    if (!is_descendant(k, cs, target)) {
        return JOIST_E_RIGHTS;
    }
    joist__space_destroy(k, target);
    (void)joist__drain_pending(k);
    return JOIST_OK;
}

joist_err joist_task_descendant(struct joist_kernel *k, joist_cspace cs, joist_cspace from,
                                joist_cspace *out)
{
    joist_err e = space_live(k, cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (from > JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    for (uint32_t s = from; s < JOIST_MAX_CSPACES; s++) {
        JOIST_WORK(k);
        if (joist__usable(k, s) && is_descendant(k, cs, s)) {
            *out = s;
            return JOIST_OK;
        }
    }
    return JOIST_E_EMPTY;
}

joist_err joist_thread_spawn(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                             joist_slot dst, joist_thread *out)
{
    uint32_t tobj;
    uint32_t obj;
    uint32_t t = JOIST_MAX_THREADS;
    joist_err e = space_live(k, cs);
    if (e == JOIST_OK) {
        e = control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &tobj);
    }
    if (e != JOIST_OK) {
        return e;
    }
    const struct joist_cap *d = joist__slot_c(k, cs, dst);
    if (d == NULL) {
        return JOIST_E_RANGE;
    }
    if (d->obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    if (k->cs[cs].quota == 0) {
        return JOIST_E_NOMEM;
    }
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        JOIST_WORK(k);
        if (k->th[i].state == JOIST_T_FREE) {
            t = i;
            break;
        }
    }
    if (t == JOIST_MAX_THREADS || !free_object(k, &obj)) {
        return JOIST_E_NOMEM;
    }
    uint32_t space = k->obj[tobj].target - 1u;
    install(k, cs, dst, obj, JOIST_OBJ_THREAD, t + 1u);
    struct joist_thread_rec *r = &k->th[t];
    uint32_t seq = r->call_seq; /* survives slot reuse (see ipc.c) */
    memset(r, 0, sizeof *r);
    r->call_seq = seq;
    r->state = JOIST_T_RUNNING;
    r->cs = space;
    r->deadline = JOIST_FOREVER;
    r->ready_seq = ++k->ready_seq;
    k->resched = 1;
    r->obj = obj + 1u;
    *out = t;
    return JOIST_OK;
}

joist_err joist_thread_kill(struct joist_kernel *k, joist_cspace cs, joist_slot slot)
{
    uint32_t obj;
    joist_err e = space_live(k, cs);
    if (e == JOIST_OK) {
        e = control_cap(k, cs, slot, JOIST_OBJ_THREAD, &obj);
    }
    if (e != JOIST_OK) {
        return e;
    }
    return joist_thread_destroy(k, k->obj[obj].target - 1u);
}

/* ---- invariants ---- */

unsigned joist__check_task(const struct joist_kernel *k, char *msg, unsigned msglen, int msg_free)
{
    unsigned bad = 0;
    uint64_t sum = 0;

#define VIOLATION(...)                                                                             \
    do {                                                                                           \
        if (bad++ == 0 && msg_free && msg != NULL && msglen > 0) {                                 \
            JOIST_FMT(msg, msglen, __VA_ARGS__);                                              \
        }                                                                                          \
    } while (0)

    /* 0016: the work list holds exactly the dying spaces, once each. */
    if (k->npending > JOIST_MAX_CSPACES) {
        VIOLATION("0016: work list length %u", k->npending);
    }
    for (uint32_t i = 0; i < k->npending && i < JOIST_MAX_CSPACES; i++) {
        uint32_t ps = k->pending[i];
        if (ps >= JOIST_MAX_CSPACES || !k->cs[ps].live || !k->cs[ps].dying) {
            VIOLATION("0016: work list entry %u is space %u, not a dying one", i, ps);
            continue;
        }
        for (uint32_t j = i + 1u; j < k->npending && j < JOIST_MAX_CSPACES; j++) {
            if (k->pending[j] == ps) {
                VIOLATION("0016: space %u is on the work list twice", ps);
            }
        }
    }
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        const struct joist_cspace_state *st = &k->cs[s];
        sum += st->charged;
        if (!st->live) {
            if (st->quota != 0 || st->parent != 0 || st->task_obj != 0 || st->fixture ||
                st->dying || st->sweep != 0) {
                VIOLATION("I3: dead space %u still has quota %u, parent %u or task %u", s,
                          st->quota, st->parent, st->task_obj);
            }
            continue;
        }
        sum += st->quota;
        if (st->dying) {
            /* 0016, and TLA+'s rule: a dying space is on the work list, has
             * no task object or threads, and nothing has been put into a
             * slot its sweep has passed (that capability would leak). */
            int listed = 0;
            for (uint32_t i = 0; i < k->npending && i < JOIST_MAX_CSPACES; i++) {
                listed |= k->pending[i] == s;
            }
            if (!listed) {
                VIOLATION("0016: dying space %u is not on the work list", s);
            }
            if (st->task_obj != 0) {
                VIOLATION("0016: dying space %u still has a task object", s);
            }
            for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
                if (k->th[t].state != JOIST_T_FREE && k->th[t].cs == s) {
                    VIOLATION("0016: dying space %u still owns thread %u", s, t);
                }
            }
            if (st->sweep > JOIST_CSPACE_SLOTS) {
                VIOLATION("0016: dying space %u sweep cursor %u", s, st->sweep);
            }
            for (uint32_t i = 0; i < st->sweep && i < JOIST_CSPACE_SLOTS; i++) {
                const struct joist_cap *c = joist__slot_c(k, s, i);
                if (c != NULL && c->obj != 0) {
                    VIOLATION("TLA+ SpaceEmptied: dying space %u has a capability in swept slot %u",
                              s, i);
                    break;
                }
            }
        } else if (st->sweep != 0) {
            VIOLATION("0016: space %u isn't dying but has sweep cursor %u", s, st->sweep);
        }
        /* The parent chain is live and acyclic. */
        /* 0004 amendment: every task but the root has an ancestor who can
         * reap it. (Fixtures, made by the test-only joist_cspace_create,
         * have none, and before boot there is no root to descend from.) */
        if (k->root != 0 && k->root != s + 1u && !st->fixture && st->parent == 0) {
            VIOLATION("I1: task %u has no parent: no ancestor can reap it", s);
        }
        uint32_t p = st->parent;
        uint32_t steps = 0;
        while (p != 0) {
            if (p > JOIST_MAX_CSPACES || !k->cs[p - 1u].live || ++steps > JOIST_MAX_CSPACES) {
                VIOLATION("I3: space %u has a dead, invalid or cyclic parent chain", s);
                break;
            }
            p = k->cs[p - 1u].parent;
        }
        /* I1: a space nothing can name is destroyed, not left running. */
        if (st->task_obj == 0 && !st->fixture && !st->dying && k->root != s + 1u) {
            VIOLATION("I1: space %u is alive but no task object names it", s);
        }
        /* Every index is range-checked before use: the checker must report a
         * corrupt state, not read outside the tables (found by CBMC). */
        if (st->task_obj > JOIST_MAX_OBJECTS) {
            VIOLATION("I1: space %u names task object %u out of range", s, st->task_obj);
        } else if (st->task_obj != 0) {
            const struct joist_object *o = &k->obj[st->task_obj - 1u];
            if (o->type != JOIST_OBJ_TASK || o->dead || o->target != s + 1u) {
                VIOLATION("I1: space %u task object %u doesn't name it back", s, st->task_obj - 1u);
            }
        }
    }
    if (k->root > JOIST_MAX_CSPACES ||
        (k->root != 0 && (!k->cs[k->root - 1u].live || k->cs[k->root - 1u].parent != 0))) {
        VIOLATION("0013: the root space is dead or has a parent");
    }
    if (sum != k->budget) {
        VIOLATION("I3: quota not conserved: live quota + charges = %llu, budget %llu",
                  (unsigned long long)sum, (unsigned long long)k->budget);
    }
    /* The budget fits the 32-bit quota counters, so no space's quota or
     * charges, each a share of it, can wrap on a refund (CBMC found a
     * fixture-fed wrap in cspace_shrink when this was missing). */
    if (k->budget > UINT32_MAX) {
        VIOLATION("I3: budget %llu exceeds the 32-bit quota counters",
                  (unsigned long long)k->budget);
    }
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *ob = &k->obj[o];
        if (ob->type == JOIST_OBJ_TASK && !ob->dead) {
            if (ob->target == 0 || ob->target > JOIST_MAX_CSPACES || !k->cs[ob->target - 1u].live ||
                k->cs[ob->target - 1u].task_obj != o + 1u) {
                VIOLATION("I1: live task object %u doesn't name a live space naming it", o);
            }
        }
        if (ob->type == JOIST_OBJ_THREAD && !ob->dead) {
            if (ob->target == 0 || ob->target > JOIST_MAX_THREADS ||
                k->th[ob->target - 1u].state == JOIST_T_FREE ||
                k->th[ob->target - 1u].obj != o + 1u) {
                VIOLATION("I1: live thread object %u doesn't name a live thread naming it", o);
            }
        }
        /* (A dead task or thread object still named by a live space or
         * thread is found from their side: each live space's and thread's
         * own object must be live and name it back, above and below. One
         * pass, not one for each dead object.) */
    }
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        const struct joist_thread_rec *r = &k->th[t];
        if (r->state != JOIST_T_FREE && r->obj > JOIST_MAX_OBJECTS) {
            VIOLATION("I1: thread %u names object %u out of range", t, r->obj);
        } else if (r->state != JOIST_T_FREE && r->obj != 0) {
            const struct joist_object *o = &k->obj[r->obj - 1u];
            if (o->type != JOIST_OBJ_THREAD || o->dead || o->target != t + 1u) {
                VIOLATION("I1: thread %u object %u doesn't name it back", t, r->obj - 1u);
            }
        }
    }
#undef VIOLATION
    return bad;
}
