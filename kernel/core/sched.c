/* sched.c - scheduling contexts and their donation (Phase 3 step 3; plan
 * §5.3, §5.6; docs/phase3-plan.md, choice 9; invariant I11).
 *
 * A scheduling context is an object holding a priority, a budget and a
 * period; a thread runs only on one. The kernel's scheduler (step 5) picks
 * threads by their run_sc's priority and charges its budget; this file only
 * keeps who holds which context, which is the part with semantics:
 *
 * - Binding: a context bound to a thread is its own (sc). A thread without
 *   one is passive: it runs only on time lent to it.
 * - Donation: a Call delivered to a passive receiver lends the caller's
 *   run_sc to it (§5.3: the server runs at the client's priority, on the
 *   client's budget). The lender's sc_to and the borrower's sc_from link
 *   them; a borrower that calls another passive server lends it on, so a
 *   context can travel down a chain.
 * - Return: a lender gets its context back when it wakes, for any reason
 *   (reply, timeout, peer death), from wherever along its chain it is; a
 *   borrower gives it back when it stops serving: when it receives or waits
 *   again, or dies. So a deferred reply never strands a client's time.
 *
 * Everything here is O(1) except the chain walks, bounded by
 * JOIST_MAX_THREADS (a chain visits each thread at most once, I11). */
#include "internal.h"

joist_err joist__control_cap(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                             joist_obj_type type, uint32_t *obj)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    if (!k->cs[cs].live) {
        return JOIST_E_DEAD;
    }
    const struct joist_cap *c = joist__slot_c(k, cs, slot);
    if (c == NULL) {
        return JOIST_E_RANGE;
    }
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (c->type != (uint8_t)type) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & JOIST_R_CONTROL) == 0) {
        return JOIST_E_RIGHTS;
    }
    if (k->obj[c->obj - 1u].dead) {
        return JOIST_E_GONE; /* a dead thread or task */
    }
    *obj = c->obj - 1u;
    return JOIST_OK;
}

joist_err joist_sc_create(struct joist_kernel *k, joist_cspace cs, joist_slot dst, uint8_t prio,
                          uint64_t budget, uint64_t period)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    struct joist_cspace_state *s = &k->cs[cs];
    if (!s->live) {
        return JOIST_E_DEAD;
    }
    struct joist_cap *d = joist__slot(k, cs, dst);
    if (d == NULL) {
        return JOIST_E_RANGE;
    }
    if (d->obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    if (budget == 0 || period == 0 || budget > period) {
        return JOIST_E_RANGE;
    }
    if (prio > s->mcp) {
        return JOIST_E_RIGHTS; /* no more priority than this space may give */
    }
    if (s->quota == 0) {
        return JOIST_E_NOMEM;
    }
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        JOIST_WORK(k);
        struct joist_object *o = &k->obj[i];
        if (o->type == JOIST_OBJ_NONE) {
            memset(o, 0, sizeof *o);
            o->type = JOIST_OBJ_SCHED;
            o->refs = 1;
            o->donor = cs;
            o->prio = prio;
            o->budget = budget;
            o->period = period;
            s->quota--;
            s->charged++;
            k->objects_live++;
            *d = (struct joist_cap){.obj = i + 1u,
                                    .type = JOIST_OBJ_SCHED,
                                    .rights = (joist_rights)(JOIST_R_CONTROL | JOIST_R_GRANT),
                                    .badge = 0};
            joist__cap_added(k, d);
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

joist_err joist_sc_bind(struct joist_kernel *k, joist_cspace cs, joist_slot sc_slot,
                        joist_slot th_slot)
{
    uint32_t so;
    uint32_t to;
    joist_err e = joist__control_cap(k, cs, sc_slot, JOIST_OBJ_SCHED, &so);
    if (e == JOIST_OK) {
        e = joist__control_cap(k, cs, th_slot, JOIST_OBJ_THREAD, &to);
    }
    if (e != JOIST_OK) {
        return e;
    }
    return joist__sc_bind_thread(k, so, k->obj[to].target - 1u);
}

joist_err joist__sc_bind_thread(struct joist_kernel *k, uint32_t so, uint32_t t)
{
    struct joist_object *o = &k->obj[so];
    struct joist_thread_rec *r = &k->th[t];
    if (o->target != 0 || r->sc != 0) {
        return JOIST_E_OCCUPIED;
    }
    o->target = t + 1u;
    r->sc = so + 1u;
    /* A thread holding or lending a borrowed context keeps to that until it
     * gives it back; then it runs on its own (giveback, reclaim). */
    if (r->run_sc == 0 && r->sc_from == 0 && r->sc_to == 0) {
        r->run_sc = so + 1u;
    }
    return JOIST_OK;
}

joist_err joist_sc_unbind(struct joist_kernel *k, joist_cspace cs, joist_slot sc_slot)
{
    uint32_t so;
    joist_err e = joist__control_cap(k, cs, sc_slot, JOIST_OBJ_SCHED, &so);
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_object *o = &k->obj[so];
    if (o->target == 0) {
        return JOIST_E_STATE; /* not bound */
    }
    struct joist_thread_rec *r = &k->th[o->target - 1u];
    if (r->sc_to != 0 && r->sc_from == 0) {
        return JOIST_E_STATE; /* its own context is lent out right now */
    }
    if (r->run_sc == so + 1u) {
        r->run_sc = 0;
    }
    r->sc = 0;
    o->target = 0;
    return JOIST_OK;
}

joist_err joist_task_set_mcp(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                             uint8_t mcp)
{
    uint32_t tobj;
    joist_err e = joist__control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &tobj);
    if (e != JOIST_OK) {
        return e;
    }
    if (mcp > k->cs[cs].mcp) {
        return JOIST_E_RIGHTS; /* never more than the caller's own */
    }
    k->cs[k->obj[tobj].target - 1u].mcp = mcp;
    return JOIST_OK;
}

/* ---- donation ---- */

void joist__sc_donate(struct joist_kernel *k, uint32_t from, uint32_t to)
{
    struct joist_thread_rec *f = &k->th[from];
    struct joist_thread_rec *r = &k->th[to];
    /* Only to a passive receiver holding nothing, from a caller running on
     * something and not already lending. */
    if (r->sc != 0 || r->run_sc != 0 || r->sc_from != 0 || r->sc_to != 0 || f->run_sc == 0 ||
        f->sc_to != 0) {
        return;
    }
    r->run_sc = f->run_sc;
    f->run_sc = 0;
    f->sc_to = to + 1u;
    r->sc_from = from + 1u;
}

void joist__sc_reclaim(struct joist_kernel *k, uint32_t x)
{
    struct joist_thread_rec *r = &k->th[x];
    uint32_t y = r->sc_to;
    uint32_t sc = 0;
    r->sc_to = 0;
    /* Down the chain to whoever holds it now, unlinking as we go; each
     * borrower on the way falls back to its own context, if any. */
    for (uint32_t n = 0; y != 0 && n < JOIST_MAX_THREADS; n++) {
        JOIST_WORK(k);
        struct joist_thread_rec *b = &k->th[y - 1u];
        uint32_t next = b->sc_to;
        if (next == 0) {
            sc = b->run_sc;
        }
        b->sc_from = 0;
        b->sc_to = 0;
        b->run_sc = b->sc;
        y = next;
    }
    r->run_sc = sc != 0 ? sc : r->sc;
}

void joist__sc_giveback(struct joist_kernel *k, uint32_t h)
{
    struct joist_thread_rec *r = &k->th[h];
    if (r->sc_from == 0) {
        return; /* holds nothing borrowed */
    }
    if (r->sc_to != 0) {
        joist__sc_reclaim(k, h); /* lent on: pull it back here first */
    }
    struct joist_thread_rec *l = &k->th[r->sc_from - 1u];
    l->run_sc = r->run_sc;
    l->sc_to = 0;
    r->sc_from = 0;
    r->run_sc = r->sc;
}

void joist__sc_thread_gone(struct joist_kernel *k, uint32_t t)
{
    struct joist_thread_rec *r = &k->th[t];
    joist__sc_giveback(k, t); /* a borrowed context goes home */
    if (r->sc_to != 0) {
        joist__sc_reclaim(k, t); /* its own comes back, to be dropped */
    }
    if (r->sc != 0) {
        k->obj[r->sc - 1u].target = 0; /* the context outlives its thread, unbound */
    }
    r->sc = 0;
    r->run_sc = 0;
}

void joist__sc_freed(struct joist_kernel *k, uint32_t obj)
{
    struct joist_object *o = &k->obj[obj];
    if (o->target != 0) {
        k->th[o->target - 1u].sc = 0;
        o->target = 0;
    }
    for (uint32_t h = 0; h < JOIST_MAX_THREADS; h++) {
        JOIST_WORK(k);
        if (k->th[h].state == JOIST_T_FREE || k->th[h].run_sc != obj + 1u) {
            continue;
        }
        /* Unwind the chain that carried it, back from its holder to the
         * thread that lent it first; everyone on it falls back to its own. */
        uint32_t y = h;
        for (uint32_t n = 0; n < JOIST_MAX_THREADS; n++) {
            JOIST_WORK(k);
            struct joist_thread_rec *b = &k->th[y];
            uint32_t from = b->sc_from;
            b->sc_from = 0;
            b->sc_to = 0;
            b->run_sc = b->sc;
            if (from == 0) {
                break;
            }
            y = from - 1u;
        }
        break; /* at most one thread runs on it (I11) */
    }
}

/* ---- invariants (I11) ---- */

unsigned joist__check_sched(const struct joist_kernel *k, char *msg, unsigned msglen, int msg_free)
{
    unsigned bad = 0;
#define VIOLATION(...)                                                                             \
    do {                                                                                           \
        if (bad++ == 0 && msg_free && msg != NULL && msglen > 0) {                                 \
            JOIST_FMT(msg, msglen, __VA_ARGS__);                                                   \
        }                                                                                          \
    } while (0)

    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *ob = &k->obj[o];
        if (ob->type != JOIST_OBJ_SCHED) {
            continue;
        }
        if (ob->budget == 0 || ob->budget > ob->period) {
            VIOLATION("I11: context %u budget %llu period %llu", o,
                      (unsigned long long)ob->budget, (unsigned long long)ob->period);
        }
        if (ob->target != 0 &&
            (ob->target > JOIST_MAX_THREADS || k->th[ob->target - 1u].sc != o + 1u)) {
            VIOLATION("I11: context %u bound to thread %u, which doesn't own it", o,
                      ob->target - 1u);
        }
    }
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        const struct joist_thread_rec *r = &k->th[t];
        if (r->state == JOIST_T_FREE) {
            if (r->sc != 0 || r->run_sc != 0 || r->sc_from != 0 || r->sc_to != 0) {
                VIOLATION("I11: free thread %u still has scheduling links", t);
            }
            continue;
        }
        const uint32_t named[2] = {r->sc, r->run_sc};
        for (unsigned i = 0; i < 2; i++) {
            if (named[i] != 0 &&
                (named[i] > JOIST_MAX_OBJECTS || k->obj[named[i] - 1u].type != JOIST_OBJ_SCHED)) {
                VIOLATION("I11: thread %u names %u, not a scheduling context", t, named[i]);
            }
        }
        if (r->sc != 0 && r->sc <= JOIST_MAX_OBJECTS && k->obj[r->sc - 1u].target != t + 1u) {
            VIOLATION("I11: thread %u owns context %u, which names another owner", t, r->sc - 1u);
        }
        if (r->run_sc != 0) {
            for (uint32_t u = t + 1u; u < JOIST_MAX_THREADS; u++) {
                if (k->th[u].state != JOIST_T_FREE && k->th[u].run_sc == r->run_sc) {
                    VIOLATION("I11: threads %u and %u both run on context %u", t, u,
                              r->run_sc - 1u);
                }
            }
        }
        if (r->sc_to != 0) {
            if (r->sc_to > JOIST_MAX_THREADS || k->th[r->sc_to - 1u].sc_from != t + 1u) {
                VIOLATION("I11: thread %u lends to %u, which doesn't borrow from it", t,
                          r->sc_to - 1u);
            }
            if (r->run_sc != 0 || r->state != JOIST_T_BLOCKED_REPLY) {
                VIOLATION("I11: lender %u runs on %u in state %u (must be waiting for a reply)",
                          t, r->run_sc, r->state);
            }
            /* The chain ends, at a holder, within the thread table. */
            uint32_t y = r->sc_to;
            uint32_t n = 0;
            while (y != 0 && y <= JOIST_MAX_THREADS && n <= JOIST_MAX_THREADS &&
                   k->th[y - 1u].sc_to != 0) {
                y = k->th[y - 1u].sc_to;
                n++;
            }
            if (y == 0 || y > JOIST_MAX_THREADS || n > JOIST_MAX_THREADS ||
                k->th[y - 1u].run_sc == 0) {
                VIOLATION("I11: thread %u's loan doesn't end at a holder", t);
            }
        }
        if (r->sc_from != 0 &&
            (r->sc_from > JOIST_MAX_THREADS || k->th[r->sc_from - 1u].sc_to != t + 1u)) {
            VIOLATION("I11: thread %u borrows from %u, which doesn't lend to it", t,
                      r->sc_from - 1u);
        }
    }
#undef VIOLATION
    return bad;
}
