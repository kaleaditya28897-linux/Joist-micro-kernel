/* task.c - tasks and threads as capability-named objects, boot, and task
 * destruction (Phase 1 work item 4; invariants I1, I3; decisions 0004, 0010,
 * 0013).
 *
 * Destruction cascades: destroying a task deletes its capabilities, and if
 * one was the last capability to another live task, that task becomes
 * unreachable and must go too. That is done with a work list (k->pending),
 * drained by a loop, never by recursion; each space is destroyed once. The
 * total work is bounded by the table sizes but not per operation: this is the
 * case decision 0003 means by "revoke may block the caller", and the kernel
 * will have to make it preemptible. Recorded as open in docs/phase1-plan.md. */
#include "internal.h"

#include <stdio.h>
#include <string.h>

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

void joist__drain_pending(struct joist_kernel *k)
{
    while (k->npending != 0) {
        JOIST_WORK(k);
        uint32_t s = k->pending[--k->npending];
        joist__space_destroy(k, s);
    }
}

void joist__space_destroy(struct joist_kernel *k, uint32_t s)
{
    struct joist_cspace_state *st = &k->cs[s];
    if (!st->live) {
        return;
    }
    /* Dead first, so deleting this task's own last capability during the
     * teardown below does not queue it again. */
    if (st->task_obj != 0) {
        k->obj[st->task_obj - 1u].dead = 1;
        st->task_obj = 0;
    }
    /* Threads before capabilities: they may hold reply capabilities whose
     * callers must wake. */
    joist__threads_destroy_owned(k, s);
    for (joist_slot i = 0; i < JOIST_CSPACE_SLOTS; i++) {
        JOIST_WORK(k);
        if (st->slot[i].obj != 0) {
            joist__cap_delete_slot(k, s, i); /* may queue unreachable tasks */
        }
    }
    /* I3: hand everything this space still owns to its parent, and its
     * children too, so no quota is lost and the parent chain stays live. */
    uint32_t p = st->parent;
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        JOIST_WORK(k);
        if (k->cs[c].live && k->cs[c].parent == s + 1u) {
            k->cs[c].parent = p;
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
    st->live = 0;
}

/* The last capability to a task or thread object is gone. */
void joist__lifecycle_freed(struct joist_kernel *k, uint32_t obj)
{
    struct joist_object *o = &k->obj[obj];
    if (o->type == JOIST_OBJ_TASK && !o->dead && o->target != 0) {
        /* Unreachable now: destroy it, from the work list. The space's
         * back-pointer is cleared first, because this object is about to be
         * freed and its slot may be reused. */
        k->cs[o->target - 1u].task_obj = 0;
        pending_push(k, o->target - 1u);
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
    k->budget = budget;
    k->root = 1;
    *root = 0;
    return JOIST_OK;
}

static joist_err space_live(const struct joist_kernel *k, joist_cspace cs)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    return k->cs[cs].live ? JOIST_OK : JOIST_E_DEAD;
}

static int free_object(struct joist_kernel *k, uint32_t *out)
{
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        JOIST_WORK(k);
        if (k->obj[i].type == JOIST_OBJ_NONE) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

/* Install a new object and its capability (all checks already done). */
static void install(struct joist_kernel *k, joist_cspace cs, joist_slot dst, uint32_t obj,
                    joist_obj_type type, uint32_t target)
{
    struct joist_object *o = &k->obj[obj];
    memset(o, 0, sizeof *o);
    o->type = (uint8_t)type;
    o->refs = 1;
    o->donor = cs;
    o->target = target;
    k->cs[cs].quota--;
    k->cs[cs].charged++;
    k->objects_live++;
    k->cs[cs].slot[dst] =
        (struct joist_cap){.obj = obj + 1u,
                           .type = (uint8_t)type,
                           .rights = (joist_rights)(JOIST_R_CONTROL | JOIST_R_GRANT),
                           .badge = 0};
    joist__cap_added(k, &k->cs[cs].slot[dst]);
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
    if (dst >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    if (k->cs[cs].slot[dst].obj != 0) {
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
    n->task_obj = obj + 1u;
    k->cs[cs].quota -= donation;
    return JOIST_OK;
}

/* Look up a live task or thread capability with CONTROL. */
static joist_err control_cap(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                             joist_obj_type type, uint32_t *obj)
{
    if (slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *c = &k->cs[cs].slot[slot];
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
    joist__space_destroy(k, k->obj[obj].target - 1u);
    joist__drain_pending(k);
    return JOIST_OK;
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
    if (dst >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    if (k->cs[cs].slot[dst].obj != 0) {
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
            (void)snprintf(msg, msglen, __VA_ARGS__);                                              \
        }                                                                                          \
    } while (0)

    if (k->npending != 0) {
        VIOLATION("I1: %u task destructions left pending", k->npending);
    }
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        const struct joist_cspace_state *st = &k->cs[s];
        sum += st->charged;
        if (!st->live) {
            if (st->quota != 0 || st->parent != 0 || st->task_obj != 0 || st->fixture) {
                VIOLATION("I3: dead space %u still has quota %u, parent %u or task %u", s,
                          st->quota, st->parent, st->task_obj);
            }
            continue;
        }
        sum += st->quota;
        /* The parent chain is live and acyclic. */
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
        if (st->task_obj == 0 && !st->fixture && k->root != s + 1u) {
            VIOLATION("I1: space %u is alive but no task object names it", s);
        }
        if (st->task_obj != 0) {
            const struct joist_object *o = &k->obj[st->task_obj - 1u];
            if (o->type != JOIST_OBJ_TASK || o->dead || o->target != s + 1u) {
                VIOLATION("I1: space %u task object %u doesn't name it back", s, st->task_obj - 1u);
            }
        }
    }
    if (k->root != 0 && (!k->cs[k->root - 1u].live || k->cs[k->root - 1u].parent != 0)) {
        VIOLATION("0013: the root space is dead or has a parent");
    }
    if (sum != k->budget) {
        VIOLATION("I3: quota not conserved: live quota + charges = %llu, budget %llu",
                  (unsigned long long)sum, (unsigned long long)k->budget);
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
        if ((ob->type == JOIST_OBJ_TASK || ob->type == JOIST_OBJ_THREAD) && ob->dead) {
            for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
                if (k->cs[s].live && k->cs[s].task_obj == o + 1u) {
                    VIOLATION("I1: dead task object %u still named by live space %u", o, s);
                }
            }
            for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
                if (k->th[t].state != JOIST_T_FREE && k->th[t].obj == o + 1u) {
                    VIOLATION("I1: dead thread object %u still names live thread %u", o, t);
                }
            }
        }
    }
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        const struct joist_thread_rec *r = &k->th[t];
        if (r->state != JOIST_T_FREE && r->obj != 0) {
            const struct joist_object *o = &k->obj[r->obj - 1u];
            if (o->type != JOIST_OBJ_THREAD || o->dead || o->target != t + 1u) {
                VIOLATION("I1: thread %u object %u doesn't name it back", t, r->obj - 1u);
            }
        }
    }
#undef VIOLATION
    return bad;
}
