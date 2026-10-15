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

static void rq_remove(struct joist_kernel *k, uint32_t t);

/* Has priority p a run queue? (All 256 have, unless the model is built
 * with fewer: JOIST_PRIOS.) */
static int prio_ok(uint32_t p)
{
    return p < JOIST_PRIOS;
}

joist_err joist_sc_create(struct joist_kernel *k, joist_cspace cs, joist_slot dst, uint8_t prio,
                          uint64_t budget, uint64_t period)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    struct joist_cspace_state *s = &k->cs[cs];
    if (!joist__usable(k, cs)) {
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
    if (prio > s->mcp || !prio_ok(prio)) {
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
            o->remaining = budget; /* the first period starts when it first runs */
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
        r->ready_seq = ++k->ready_seq; /* ready on its own time: behind its equals */
        k->resched = 1;                /* it can run now */
        joist__rq_sync(k, t);
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
        k->resched = 1; /* it can't run any more: its CPU wants another */
        joist__rq_sync(k, o->target - 1u);
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
    joist__rq_sync(k, from);
    joist__rq_sync(k, to);
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
        uint32_t bt = y - 1u;
        b->sc_from = 0;
        b->sc_to = 0;
        b->run_sc = b->sc; /* (its place stays: the reply, ipc.c, gives it a new one) */
        joist__rq_sync(k, bt);
        y = next;
    }
    r->run_sc = sc != 0 ? sc : r->sc;
    joist__rq_sync(k, x);
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
    uint32_t lt = r->sc_from - 1u;
    struct joist_thread_rec *l = &k->th[lt];
    l->run_sc = r->run_sc;
    l->sc_to = 0;
    r->sc_from = 0;
    r->run_sc = r->sc;
    joist__rq_sync(k, lt);
    joist__rq_sync(k, h);
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
    rq_remove(k, t);
    r->sc = 0;
    r->run_sc = 0;
}

void joist__sc_freed(struct joist_kernel *k, uint32_t obj)
{
    struct joist_object *o = &k->obj[obj];
    k->resched = 1; /* whoever ran on it can't; borrowers fall back to their own */
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
            b->ready_seq = ++k->ready_seq; /* on its own again: behind its equals */
            joist__rq_sync(k, y);
            if (from == 0) {
                break;
            }
            y = from - 1u;
        }
        break; /* at most one thread runs on it (I11) */
    }
}

/* ---- who runs (Phase 3 step 5) ---- */

/* Can thread t run? Its context, if so: it is running (not blocked or
 * suspended), runs on a context, and that context has budget left or was
 * due a new period at the last tick. */
static const struct joist_object *can_run(const struct joist_kernel *k, uint32_t t)
{
    const struct joist_thread_rec *r = &k->th[t];
    if (r->state != JOIST_T_RUNNING || r->run_sc == 0) {
        return NULL;
    }
    const struct joist_object *sc = &k->obj[r->run_sc - 1u];
    return sc->remaining != 0 || sc->refill_at <= k->tick_now ? sc : NULL;
}

/* Is (prio a, place sa) to go before (prio b, place sb)? */
static int before(uint8_t a, uint64_t sa, uint8_t b, uint64_t sb)
{
    return a > b || (a == b && sa < sb);
}

/* ---- the run queues (choice 9; Phase 3 step 10) ---- */

static void rq_remove(struct joist_kernel *k, uint32_t t)
{
    struct joist_thread_rec *r = &k->th[t];
    if (r->rq_on == 0 || r->rq_on > JOIST_MAX_CPUS) {
        return;
    }
    struct joist_runq *q = &k->rq[r->rq_on - 1u];
    uint8_t p = r->rq_prio;
    if (r->rq_prev != 0) {
        k->th[r->rq_prev - 1u].rq_next = r->rq_next;
    } else {
        q->list[p].head = r->rq_next;
    }
    if (r->rq_next != 0) {
        k->th[r->rq_next - 1u].rq_prev = r->rq_prev;
    } else {
        q->list[p].tail = r->rq_prev;
    }
    if (q->list[p].head == 0) {
        q->bits[p / 64u] &= ~(1ull << (p % 64u));
    }
    r->rq_on = 0;
    r->rq_prev = 0;
    r->rq_next = 0;
}

/* Insert in its place: after every thread of its priority with an older
 * place. Nearly always at the tail (a new place is the newest), or at the
 * head (a place taken from a thread just running, which was first in line):
 * both O(1). Anything else walks back from the tail, and every step is
 * counted (k->rq_walks). */
static void rq_insert(struct joist_kernel *k, uint32_t t)
{
    struct joist_thread_rec *r = &k->th[t];
    uint32_t cpu = r->home < JOIST_MAX_CPUS ? r->home : 0;
    struct joist_runq *q = &k->rq[cpu];
    uint8_t p = k->obj[r->run_sc - 1u].prio;
    uint32_t h = q->list[p].head;
    uint32_t after = 0; /* insert after this one (thread plus one; 0: at the head) */
    if (h != 0 && k->th[h - 1u].ready_seq < r->ready_seq) {
        after = q->list[p].tail; /* not before the head: so from the tail back */
        for (uint32_t n = 0; k->th[after - 1u].ready_seq > r->ready_seq && n < JOIST_MAX_THREADS;
             n++) {
            after = k->th[after - 1u].rq_prev;
            k->rq_walks++;
        }
    }
    r->rq_prev = after;
    r->rq_next = after != 0 ? k->th[after - 1u].rq_next : q->list[p].head;
    if (r->rq_prev != 0) {
        k->th[r->rq_prev - 1u].rq_next = t + 1u;
    } else {
        q->list[p].head = t + 1u;
    }
    if (r->rq_next != 0) {
        k->th[r->rq_next - 1u].rq_prev = t + 1u;
    } else {
        q->list[p].tail = t + 1u;
    }
    q->bits[p / 64u] |= 1ull << (p % 64u);
    r->rq_on = (uint8_t)(cpu + 1u);
    r->rq_prio = p;
}

/* Is queued thread t where it belongs: its CPU's queue, its priority's
 * list, between an older place and a newer? */
static int rq_in_place(const struct joist_kernel *k, uint32_t t, uint8_t prio)
{
    const struct joist_thread_rec *r = &k->th[t];
    uint32_t cpu = r->home < JOIST_MAX_CPUS ? r->home : 0;
    return r->rq_on == cpu + 1u && r->rq_prio == prio &&
           (r->rq_prev == 0 || k->th[r->rq_prev - 1u].ready_seq < r->ready_seq) &&
           (r->rq_next == 0 || k->th[r->rq_next - 1u].ready_seq > r->ready_seq);
}

static void rq_file(struct joist_kernel *k, uint32_t t)
{
    const struct joist_object *sc = k->th[t].cpu == 0 ? can_run(k, t) : NULL;
    if (sc != NULL && k->th[t].rq_on != 0 && rq_in_place(k, t, sc->prio)) {
        return; /* nothing to do: the common case for a thread already waiting */
    }
    rq_remove(k, t);
    if (sc != NULL) {
        rq_insert(k, t);
    }
}

void joist__rq_file(struct joist_kernel *k, uint32_t t)
{
    if (t < JOIST_MAX_THREADS) {
        rq_file(k, t);
    }
}

/* File every noted thread (the hold, if any, stays): first out of the
 * lists, each that is no longer where it belongs, so that the lists hold
 * only threads in their places; then each where it belongs now. */
static void rq_flush(struct joist_kernel *k)
{
    uint32_t n = k->rq_npend < JOIST_MAX_THREADS ? k->rq_npend : JOIST_MAX_THREADS;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = k->rq_pend[i];
        if (k->th[t].rq_on == 0) {
            continue;
        }
        const struct joist_object *sc = k->th[t].cpu == 0 ? can_run(k, t) : NULL;
        if (sc == NULL || !rq_in_place(k, t, sc->prio)) {
            rq_remove(k, t);
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = k->rq_pend[i];
        struct joist_thread_rec *r = &k->th[t];
        r->rq_dirty = 0;
        if (r->rq_on == 0 && (r->cpu != 0 || r->state != JOIST_T_RUNNING)) {
            continue; /* on a CPU, or waiting: on no list, as it should be (the direct switch) */
        }
        rq_file(k, t);
    }
    k->rq_npend = 0;
}

void joist__sched_flush(struct joist_kernel *k)
{
    rq_flush(k);
}

/* The first thread of the highest priority on CPU q's queue, or
 * JOIST_MAX_THREADS. O(JOIST_PRIO_WORDS). */
static uint32_t rq_head(const struct joist_kernel *k, uint32_t q)
{
    for (uint32_t w = JOIST_PRIO_WORDS; w-- > 0;) {
        uint64_t b = k->rq[q].bits[w];
        if (b != 0) {
            uint32_t p = w * 64u + (63u - (uint32_t)__builtin_clzll(b));
            return k->rq[q].list[p].head - 1u;
        }
    }
    return JOIST_MAX_THREADS;
}

/* What CPU `cpu` should run: its own thread, if it can still run, or the
 * best head of any queue, whichever goes first. The same as the scan
 * (best_for), in O(CPUs x priority words). */
static uint32_t best_for(const struct joist_kernel *k, uint32_t cpu);

static uint32_t best_queued(const struct joist_kernel *k, uint32_t cpu)
{
    if (k->rq_npend != 0) {
        return best_for(k, cpu); /* (held, and something noted: the queues aren't true yet) */
    }
    uint32_t best = JOIST_MAX_THREADS;
    uint8_t bp = 0;
    uint64_t bs = 0;
    uint32_t mine = k->on_cpu[cpu];
    if (mine != 0 && mine <= JOIST_MAX_THREADS) {
        const struct joist_object *sc = can_run(k, mine - 1u);
        if (sc != NULL) {
            best = mine - 1u;
            bp = sc->prio;
            bs = k->th[best].ready_seq;
        }
    }
    for (uint32_t q = 0; q < JOIST_MAX_CPUS; q++) {
        uint32_t h = rq_head(k, q);
        if (h >= JOIST_MAX_THREADS) {
            continue;
        }
        uint8_t hp = k->obj[k->th[h].run_sc - 1u].prio;
        if (best == JOIST_MAX_THREADS || before(hp, k->th[h].ready_seq, bp, bs)) {
            best = h;
            bp = hp;
            bs = k->th[h].ready_seq;
        }
    }
    return best;
}

/* The best thread CPU `cpu` could run: one that can run and is on no other
 * CPU. JOIST_MAX_THREADS if none. The scan: the specification the run
 * queues are held to (joist__check_sched, test_runq.c); not used to choose. */
static uint32_t best_for(const struct joist_kernel *k, uint32_t cpu)
{
    uint32_t best = JOIST_MAX_THREADS;
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        const struct joist_object *sc = can_run(k, i);
        if (sc == NULL || (k->th[i].cpu != 0 && k->th[i].cpu != cpu + 1u)) {
            continue;
        }
        if (best == JOIST_MAX_THREADS) {
            best = i;
            continue;
        }
        const struct joist_object *bsc = &k->obj[k->th[best].run_sc - 1u];
        if (sc->prio > bsc->prio ||
            (sc->prio == bsc->prio && k->th[i].ready_seq < k->th[best].ready_seq)) {
            best = i;
        }
    }
    return best;
}

void joist_sched_on(struct joist_kernel *k, uint32_t cpu, joist_thread t)
{
    if (cpu >= JOIST_MAX_CPUS) {
        return;
    }
    uint32_t was = k->on_cpu[cpu];
    k->on_cpu[cpu] = 0;
    if (was != 0 && was <= JOIST_MAX_THREADS) {
        k->th[was - 1u].cpu = 0;
        if (was - 1u != t) {
            joist__rq_sync(k, was - 1u); /* off this CPU: back on a queue, if it can run */
        }
    }
    if (t < JOIST_MAX_THREADS && k->th[t].state != JOIST_T_FREE) {
        uint32_t other = k->th[t].cpu;
        if (other != 0 && other <= JOIST_MAX_CPUS) {
            k->on_cpu[other - 1u] = 0; /* (never, if the caller chose rightly: I13 holds anyway) */
        }
        k->th[t].cpu = (uint8_t)(cpu + 1u);
        k->th[t].home = (uint8_t)cpu;
        k->on_cpu[cpu] = t + 1u;
        joist__rq_sync(k, t); /* on a CPU: off the queues */
    }
}

joist_err joist_sched_pick_on(struct joist_kernel *k, uint32_t cpu, joist_thread *t)
{
    if (cpu >= JOIST_MAX_CPUS) {
        return JOIST_E_RANGE;
    }
    rq_flush(k); /* (anything noted under a hold, first) */
    k->work += (uint64_t)JOIST_MAX_CPUS * JOIST_PRIO_WORDS; /* the queues' heads (I9) */
    uint32_t best = best_queued(k, cpu);
    if (best >= JOIST_MAX_THREADS) {
        joist_sched_on(k, cpu, JOIST_MAX_THREADS);
        return JOIST_E_EMPTY;
    }
    joist_sched_on(k, cpu, best);
    /* A period that is due starts now, when its context is first used. */
    struct joist_object *sc = &k->obj[k->th[best].run_sc - 1u];
    if (sc->refill_at <= k->now) {
        sc->remaining = sc->budget;
        sc->refill_at = k->now > JOIST_FOREVER - sc->period ? JOIST_FOREVER : k->now + sc->period;
    }
    *t = best;
    return JOIST_OK;
}

joist_err joist_sched_pick(struct joist_kernel *k, joist_thread *t)
{
    return joist_sched_pick_on(k, 0, t);
}

joist_err joist_yield(struct joist_kernel *k, joist_thread t)
{
    if (t >= JOIST_MAX_THREADS) {
        return JOIST_E_RANGE;
    }
    if (k->th[t].state != JOIST_T_RUNNING) {
        return JOIST_E_STATE;
    }
    k->th[t].ready_seq = ++k->ready_seq; /* behind its equals */
    k->resched = 1;                      /* an equal waiting elsewhere may want this CPU */
    joist__rq_sync(k, t);                /* to the back of its list */
    return JOIST_OK;
}

int joist_sched_better(const struct joist_kernel *k, uint32_t cpu)
{
    if (cpu >= JOIST_MAX_CPUS) {
        return 0;
    }
    uint32_t best = best_queued(k, cpu);
    return best != (k->on_cpu[cpu] != 0 ? k->on_cpu[cpu] - 1u : JOIST_MAX_THREADS);
}

void joist__off_cpu(struct joist_kernel *k, uint32_t t)
{
    struct joist_thread_rec *r = &k->th[t];
    if (r->cpu == 0) {
        return;
    }
    uint32_t cpu = r->cpu - 1u;
    if (k->hooks.off_cpu != NULL) {
        k->hooks.off_cpu(k->hooks.ctx, t, cpu);
    }
    k->on_cpu[cpu] = 0; /* off it, at both ends */
    r->cpu = 0;
    k->resched = 1; /* that CPU has nothing now */
    joist__rq_sync(k, t);
}

void joist_sched_charge(struct joist_kernel *k, joist_thread t, uint64_t used)
{
    if (t >= JOIST_MAX_THREADS || k->th[t].state == JOIST_T_FREE || k->th[t].run_sc == 0) {
        return;
    }
    struct joist_object *sc = &k->obj[k->th[t].run_sc - 1u];
    if (used >= sc->remaining) {
        sc->remaining = 0;
        k->th[t].ready_seq = ++k->ready_seq; /* its turn is over: behind its equals */
        joist__rq_sync(k, t);                /* (if it is queued: the kernel's CPU drops it later) */
    } else {
        sc->remaining -= used;
    }
}

joist_thread joist_sched_handed(const struct joist_kernel *k, joist_thread from, uint64_t place,
                                joist_thread to)
{
    if (from >= JOIST_MAX_THREADS || to >= JOIST_MAX_THREADS) {
        return JOIST_MAX_THREADS;
    }
    const struct joist_thread_rec *f = &k->th[from];
    const struct joist_thread_rec *r = &k->th[to];
    if (f->state != JOIST_T_BLOCKED_REPLY && f->state != JOIST_T_BLOCKED_RECV) {
        return JOIST_MAX_THREADS; /* it didn't block: it may be the one to go on */
    }
    if (f->deadline != JOIST_FOREVER) {
        return JOIST_MAX_THREADS; /* its wait can end by itself: a new timer event */
    }
    if (r->state != JOIST_T_RUNNING || r->run_sc == 0 || r->cpu != 0) {
        return JOIST_MAX_THREADS; /* `to` can't run, or is another CPU's */
    }
    if (r->ready_seq != place) {
        /* Not in `from`'s place in line: the context `from` ran on didn't
         * go to it (a place moves only with its context, ipc.c). */
        return JOIST_MAX_THREADS;
    }
    if (k->obj[r->run_sc - 1u].remaining == 0) {
        return JOIST_MAX_THREADS; /* the budget is spent: nobody runs on it now */
    }
    return to;
}

uint64_t joist_sched_next_event(const struct joist_kernel *k, joist_thread running)
{
    uint64_t next = JOIST_FOREVER;
    if (running < JOIST_MAX_THREADS && k->th[running].state != JOIST_T_FREE &&
        k->th[running].run_sc != 0) {
        uint64_t left = k->obj[k->th[running].run_sc - 1u].remaining;
        next = k->now > JOIST_FOREVER - left ? JOIST_FOREVER : k->now + left;
    }
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        const struct joist_thread_rec *r = &k->th[i];
        if (r->state == JOIST_T_FREE) {
            continue;
        }
        if (r->state != JOIST_T_RUNNING && r->state != JOIST_T_SUSPENDED &&
            r->deadline < next) {
            next = r->deadline; /* a blocked thread's timeout */
        }
        if (r->state == JOIST_T_RUNNING && r->run_sc != 0) {
            const struct joist_object *sc = &k->obj[r->run_sc - 1u];
            if (sc->remaining == 0 && sc->refill_at > k->tick_now && sc->refill_at < next) {
                /* Out of budget: its next period, which a tick at or after
                 * it makes count (it may be past already: then at once). */
                next = sc->refill_at;
            }
        }
    }
    return next;
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

    /* I13: who is on which CPU is recorded at both ends, and a thread on a
     * CPU exists. */
    for (uint32_t c = 0; c < JOIST_MAX_CPUS; c++) {
        uint32_t t = k->on_cpu[c];
        if (t != 0 && (t > JOIST_MAX_THREADS || k->th[t - 1u].state == JOIST_T_FREE ||
                       k->th[t - 1u].cpu != c + 1u)) {
            VIOLATION("I13: CPU %u has thread %u, which is free or doesn't say so", c, t - 1u);
        }
    }
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        uint32_t c = k->th[t].cpu;
        if (c != 0 && (k->th[t].state == JOIST_T_FREE || c > JOIST_MAX_CPUS ||
                       k->on_cpu[c - 1u] != t + 1u)) {
            VIOLATION("I13: thread %u says CPU %u, which doesn't have it", t, c - 1u);
        }
    }
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *ob = &k->obj[o];
        if (ob->type != JOIST_OBJ_SCHED) {
            continue;
        }
        if (ob->remaining > ob->budget) {
            VIOLATION("I11: context %u has %llu left of a budget of %llu", o,
                      (unsigned long long)ob->remaining, (unsigned long long)ob->budget);
        }
        if (!prio_ok(ob->prio)) {
            VIOLATION("I11: context %u has priority %u, past the queues' %u", o, ob->prio,
                      (unsigned)JOIST_PRIOS);
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
    /* I14: the run queues. A thread is on a queue exactly when it can run
     * and is on no CPU; each list is well linked, holds only its priority,
     * in order of place, and its bit says whether it has any. And what the
     * queues choose for each CPU is what the scan would. */
    if (k->rq_hold != 0 || k->rq_npend != 0) {
        VIOLATION("I14: between operations, a hold on the queues (%u, %u noted)", k->rq_hold,
                  k->rq_npend);
    }
    uint32_t queued = 0;
    for (uint32_t c = 0; c < JOIST_MAX_CPUS; c++) {
        const struct joist_runq *q = &k->rq[c];
        uint32_t used = JOIST_PRIOS - (JOIST_PRIO_WORDS - 1u) * 64u; /* bits of the last word */
        if (used < 64u && (q->bits[JOIST_PRIO_WORDS - 1u] >> used) != 0) {
            VIOLATION("I14: CPU %u has a bit for a priority past %u", c, (unsigned)JOIST_PRIOS);
        }
        for (uint32_t p = 0; p < JOIST_PRIOS; p++) {
            int bit = (int)((q->bits[p / 64u] >> (p % 64u)) & 1u);
            if (bit != (q->list[p].head != 0) || (q->list[p].head == 0) != (q->list[p].tail == 0)) {
                VIOLATION("I14: CPU %u priority %u: bit, head and tail disagree", c, p);
                continue;
            }
            uint32_t prev = 0;
            uint32_t cur = q->list[p].head;
            uint32_t n = 0;
            while (cur != 0) {
                if (cur > JOIST_MAX_THREADS || ++n > JOIST_MAX_THREADS) {
                    VIOLATION("I14: CPU %u priority %u list is corrupt or cyclic", c, p);
                    break;
                }
                const struct joist_thread_rec *r = &k->th[cur - 1u];
                if (r->rq_on != c + 1u || r->rq_prio != p || r->rq_prev != prev) {
                    VIOLATION("I14: thread %u's links disagree with CPU %u priority %u", cur - 1u,
                              c, p);
                }
                if (r->run_sc == 0 || r->run_sc > JOIST_MAX_OBJECTS ||
                    k->obj[r->run_sc - 1u].prio != p) {
                    VIOLATION("I14: thread %u on priority %u's list runs at another", cur - 1u, p);
                }
                if (prev != 0 && k->th[prev - 1u].ready_seq >= r->ready_seq) {
                    VIOLATION("I14: thread %u is ahead of %u in line, out of order", prev - 1u,
                              cur - 1u);
                }
                prev = cur;
                cur = r->rq_next;
            }
            if (cur == 0 && q->list[p].tail != prev) {
                VIOLATION("I14: CPU %u priority %u: tail is not the last", c, p);
            }
            queued += n;
        }
    }
    uint32_t eligible = 0;
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        const struct joist_thread_rec *r = &k->th[t];
        if (r->run_sc > JOIST_MAX_OBJECTS || r->rq_on > JOIST_MAX_CPUS) {
            VIOLATION("I14: thread %u's context or queue is out of range", t);
            continue;
        }
        int want = r->state != JOIST_T_FREE && r->cpu == 0 && can_run(k, t) != NULL;
        eligible += want != 0;
        if (r->rq_dirty != 0) {
            VIOLATION("I14: thread %u is still noted to be filed", t);
        }
        if (want != (r->rq_on != 0)) {
            VIOLATION("I14: thread %u %s queued but %s", t, r->rq_on != 0 ? "is" : "isn't",
                      want ? "can run on no CPU" : "can't run or is on a CPU");
        }
    }
    if (queued != eligible) {
        VIOLATION("I14: %u threads on the queues, %u that should be", queued, eligible);
    }
    /* (Only of a sound state: the choice reads the queues and the threads
     * they name without checking them again.) */
    for (uint32_t c = 0; c < JOIST_MAX_CPUS && bad == 0; c++) {
        uint32_t a = best_queued(k, c);
        uint32_t b = best_for(k, c);
        if (a != b) {
            VIOLATION("I14: for CPU %u the queues choose %u, the scan %u", c, a, b);
        }
    }
#undef VIOLATION
    return bad;
}
