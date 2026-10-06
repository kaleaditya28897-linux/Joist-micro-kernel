/* ipc.c - threads, endpoint queues and synchronous IPC (Phase 1 work item 2;
 * invariants I6, I7, I10).
 *
 * Every operation touches a fixed number of threads and objects, except the
 * wake-everyone paths (endpoint destruction, joist_tick, destroying a space's
 * threads), which are bounded by JOIST_MAX_THREADS (I9). */
#include "internal.h"

uint64_t joist__deadline(const struct joist_kernel *k, uint64_t timeout)
{
    if (timeout == JOIST_FOREVER || k->now > JOIST_FOREVER - timeout) {
        return JOIST_FOREVER;
    }
    return k->now + timeout;
}

static joist_err running(const struct joist_kernel *k, joist_thread t)
{
    if (t >= JOIST_MAX_THREADS) {
        return JOIST_E_RANGE;
    }
    return k->th[t].state == JOIST_T_RUNNING ? JOIST_OK : JOIST_E_STATE;
}

void joist__wake(struct joist_kernel *k, uint32_t t, joist_err result)
{
    struct joist_thread_rec *r = &k->th[t];
    r->state = JOIST_T_RUNNING;
    r->deadline = JOIST_FOREVER;
    r->result = result;
}

/* ---- endpoint queues: doubly linked through thread records, O(1) ---- */

void joist__q_push(struct joist_kernel *k, uint32_t obj, joist_queue_kind kind, uint32_t t)
{
    struct joist_object *e = &k->obj[obj];
    struct joist_thread_rec *r = &k->th[t];
    r->ep = obj + 1u;
    r->qnext = 0;
    r->qprev = e->qtail;
    if (e->qtail != 0) {
        k->th[e->qtail - 1u].qnext = t + 1u;
    } else {
        e->qhead = t + 1u;
        e->qkind = (uint8_t)kind;
    }
    e->qtail = t + 1u;
}

/* Remove thread t from the queue of object obj (an index), which must be the
 * queue it is on. The caller names the object rather than this function
 * reading it from the thread's ep field, so the index used is always one the
 * caller has checked. */
static void q_remove(struct joist_kernel *k, uint32_t t, uint32_t obj)
{
    struct joist_thread_rec *r = &k->th[t];
    struct joist_object *e = &k->obj[obj];
    if (r->qprev != 0) {
        k->th[r->qprev - 1u].qnext = r->qnext;
    } else {
        e->qhead = r->qnext;
    }
    if (r->qnext != 0) {
        k->th[r->qnext - 1u].qprev = r->qprev;
    } else {
        e->qtail = r->qprev;
    }
    if (e->qhead == 0) {
        e->qkind = JOIST_Q_NONE;
    }
    r->ep = 0;
    r->qprev = 0;
    r->qnext = 0;
}

uint32_t joist__q_pop(struct joist_kernel *k, uint32_t obj)
{
    uint32_t t = k->obj[obj].qhead - 1u;
    q_remove(k, t, obj);
    return t;
}

/* Look up an endpoint capability in the thread's space and check a right. */
static joist_err ep_lookup(const struct joist_kernel *k, joist_thread t, joist_slot slot,
                           joist_rights need, uint32_t *obj, joist_badge *badge)
{
    const struct joist_cspace_state *s = &k->cs[k->th[t].cs];
    if (slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *c = &s->slot[slot];
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (c->type != JOIST_OBJ_ENDPOINT) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & need) == 0) {
        return JOIST_E_RIGHTS;
    }
    if (k->obj[c->obj - 1u].dead) {
        return JOIST_E_GONE; /* I5: the server is gone; every use fails */
    }
    *obj = c->obj - 1u;
    *badge = c->badge;
    return JOIST_OK;
}

/* Hand the server a reply capability for caller c's delivered Call. */
static void mint_reply(struct joist_kernel *k, uint32_t server, uint32_t c)
{
    k->th[c].call_seq++;
    k->th[c].state = JOIST_T_BLOCKED_REPLY; /* the Call's deadline keeps running */
    k->th[server].reply_to = c + 1u;
    k->th[server].reply_seq = k->th[c].call_seq;
}

/* ---- threads ---- */

joist_err joist_thread_create(struct joist_kernel *k, joist_cspace cs, joist_thread *out)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    if (!k->cs[cs].live) {
        return JOIST_E_DEAD;
    }
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        JOIST_WORK(k);
        struct joist_thread_rec *r = &k->th[i];
        if (r->state == JOIST_T_FREE) {
            /* call_seq survives reuse of the slot, so a stale reply capability
             * held for a previous thread here can never match a new call. */
            uint32_t seq = r->call_seq;
            memset(r, 0, sizeof *r);
            r->call_seq = seq;
            r->state = JOIST_T_RUNNING;
            r->cs = cs;
            r->deadline = JOIST_FOREVER;
            *out = i;
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

joist_err joist_thread_destroy(struct joist_kernel *k, joist_thread t)
{
    if (t >= JOIST_MAX_THREADS) {
        return JOIST_E_RANGE;
    }
    struct joist_thread_rec *r = &k->th[t];
    if (r->state == JOIST_T_FREE) {
        return JOIST_E_STATE;
    }
    if ((r->state == JOIST_T_BLOCKED_SEND || r->state == JOIST_T_BLOCKED_RECV ||
         r->state == JOIST_T_BLOCKED_WAIT) &&
        r->ep != 0) {
        q_remove(k, t, r->ep - 1u);
    }
    /* A server dying with a reply capability wakes its caller (I6, I10). */
    if (r->reply_to != 0) {
        uint32_t c = r->reply_to - 1u;
        if (k->th[c].state == JOIST_T_BLOCKED_REPLY && k->th[c].call_seq == r->reply_seq) {
            joist__wake(k, c, JOIST_E_PEER);
        }
    }
    if (r->obj != 0) {
        k->obj[r->obj - 1u].dead = 1; /* thread capabilities now fail with E_GONE */
    }
    uint32_t seq = r->call_seq + 1u; /* invalidates reply capabilities naming t */
    memset(r, 0, sizeof *r);
    r->call_seq = seq;
    return JOIST_OK;
}

void joist__threads_destroy_owned(struct joist_kernel *k, joist_cspace cs)
{
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        JOIST_WORK(k);
        if (k->th[i].state != JOIST_T_FREE && k->th[i].cs == cs) {
            (void)joist_thread_destroy(k, i);
        }
    }
}

void joist__queue_wake_all(struct joist_kernel *k, uint32_t obj, joist_err r)
{
    while (k->obj[obj].qhead != 0) {
        JOIST_WORK(k);
        uint32_t t = joist__q_pop(k, obj);
        joist__wake(k, t, r);
    }
}

/* ---- IPC ---- */

joist_err joist_call(struct joist_kernel *k, joist_thread t, joist_slot slot,
                     const struct joist_msg *m, uint64_t timeout)
{
    uint32_t obj;
    joist_badge badge;
    joist_err e = running(k, t);
    if (e != JOIST_OK) {
        return e;
    }
    /* A Call always waits for its reply, so "do not block" can't be honoured. */
    if (timeout == 0) {
        return JOIST_E_TIMEOUT;
    }
    e = ep_lookup(k, t, slot, JOIST_R_SEND, &obj, &badge);
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_object *ep = &k->obj[obj];
    struct joist_thread_rec *me = &k->th[t];
    me->deadline = joist__deadline(k, timeout);
    if (ep->qkind == JOIST_Q_RECEIVERS) {
        uint32_t r = joist__q_pop(k, obj);
        k->th[r].msg = *m;
        k->th[r].badge = badge; /* I7: the badge of the capability used */
        joist__wake(k, r, JOIST_OK);
        mint_reply(k, r, t);
        return JOIST_BLOCKED;
    }
    me->msg = *m;
    me->badge = badge;
    me->state = JOIST_T_BLOCKED_SEND;
    joist__q_push(k, obj, JOIST_Q_SENDERS, t);
    return JOIST_BLOCKED;
}

joist_err joist_recv(struct joist_kernel *k, joist_thread t, joist_slot slot, uint64_t timeout)
{
    uint32_t obj;
    joist_badge badge;
    joist_err e = running(k, t);
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_thread_rec *me = &k->th[t];
    /* Receiving would overwrite an unused reply capability and strand its
     * caller (I10); the server must Reply first, or use ReplyRecv. */
    if (me->reply_to != 0) {
        return JOIST_E_STATE;
    }
    e = ep_lookup(k, t, slot, JOIST_R_RECV, &obj, &badge);
    if (e != JOIST_OK) {
        return e;
    }
    if (k->obj[obj].qkind == JOIST_Q_SENDERS) {
        uint32_t s = joist__q_pop(k, obj);
        me->msg = k->th[s].msg;
        me->badge = k->th[s].badge;
        me->result = JOIST_OK;
        mint_reply(k, t, s);
        return JOIST_OK;
    }
    if (timeout == 0) {
        return JOIST_E_TIMEOUT;
    }
    me->state = JOIST_T_BLOCKED_RECV;
    me->deadline = joist__deadline(k, timeout);
    joist__q_push(k, obj, JOIST_Q_RECEIVERS, t);
    return JOIST_BLOCKED;
}

/* Deliver a reply to caller c, if it still waits for the call numbered seq. */
static joist_err deliver_reply(struct joist_kernel *k, uint32_t c, uint32_t seq,
                               const struct joist_msg *m)
{
    if (k->th[c].state != JOIST_T_BLOCKED_REPLY || k->th[c].call_seq != seq) {
        return JOIST_E_PEER; /* the caller stopped waiting */
    }
    k->th[c].msg = *m;
    k->th[c].badge = 0;
    joist__wake(k, c, JOIST_OK);
    return JOIST_OK;
}

joist_err joist_reply(struct joist_kernel *k, joist_thread t, const struct joist_msg *m)
{
    joist_err e = running(k, t);
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_thread_rec *me = &k->th[t];
    if (me->reply_to == 0) {
        return JOIST_E_NOREPLY;
    }
    uint32_t c = me->reply_to - 1u;
    uint32_t seq = me->reply_seq;
    me->reply_to = 0; /* consumed, whatever happens next (I6) */
    me->reply_seq = 0;
    return deliver_reply(k, c, seq, m);
}

/* Look up a reply object capability in t's space, needing SEND. */
static joist_err reply_obj(const struct joist_kernel *k, joist_thread t, joist_slot slot,
                           uint32_t *obj)
{
    if (slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *c = &k->cs[k->th[t].cs].slot[slot];
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (c->type != JOIST_OBJ_REPLY) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & JOIST_R_SEND) == 0) {
        return JOIST_E_RIGHTS;
    }
    *obj = c->obj - 1u;
    return JOIST_OK;
}

joist_err joist_reply_save(struct joist_kernel *k, joist_thread t, joist_slot slot)
{
    uint32_t obj;
    joist_err e = running(k, t);
    if (e == JOIST_OK) {
        e = reply_obj(k, t, slot, &obj);
    }
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_thread_rec *me = &k->th[t];
    if (me->reply_to == 0) {
        return JOIST_E_NOREPLY;
    }
    struct joist_object *o = &k->obj[obj];
    if (o->target != 0) {
        return JOIST_E_OCCUPIED;
    }
    o->target = me->reply_to; /* moved, not copied: still exactly one (I6) */
    o->rseq = me->reply_seq;
    me->reply_to = 0;
    me->reply_seq = 0;
    return JOIST_OK;
}

joist_err joist_reply_saved(struct joist_kernel *k, joist_thread t, joist_slot slot,
                            const struct joist_msg *m)
{
    uint32_t obj;
    joist_err e = running(k, t);
    if (e == JOIST_OK) {
        e = reply_obj(k, t, slot, &obj);
    }
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_object *o = &k->obj[obj];
    if (o->target == 0) {
        return JOIST_E_NOREPLY;
    }
    uint32_t c = o->target - 1u;
    uint32_t seq = o->rseq;
    o->target = 0; /* consumed (I6) */
    o->rseq = 0;
    return deliver_reply(k, c, seq, m);
}

/* A bound reply object's last capability is gone: its caller would wait
 * forever, so it wakes now (I10). */
void joist__reply_obj_freed(struct joist_kernel *k, uint32_t obj)
{
    struct joist_object *o = &k->obj[obj];
    if (o->target != 0) {
        uint32_t c = o->target - 1u;
        if (k->th[c].state == JOIST_T_BLOCKED_REPLY && k->th[c].call_seq == o->rseq) {
            joist__wake(k, c, JOIST_E_PEER);
        }
        o->target = 0;
        o->rseq = 0;
    }
}

joist_err joist_reply_recv(struct joist_kernel *k, joist_thread t, const struct joist_msg *m,
                           joist_slot ep, uint64_t timeout)
{
    joist_err e = running(k, t);
    if (e != JOIST_OK) {
        return e;
    }
    if (k->th[t].reply_to != 0) {
        (void)joist_reply(k, t, m); /* a caller that stopped waiting is not an error */
    }
    return joist_recv(k, t, ep, timeout);
}

void joist_tick(struct joist_kernel *k, uint64_t dt)
{
    k->now = (k->now > JOIST_FOREVER - dt) ? JOIST_FOREVER : k->now + dt;
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        JOIST_WORK(k);
        struct joist_thread_rec *r = &k->th[i];
        if (r->state == JOIST_T_FREE || r->state == JOIST_T_RUNNING ||
            r->deadline == JOIST_FOREVER || r->deadline > k->now) {
            continue;
        }
        if ((r->state == JOIST_T_BLOCKED_SEND || r->state == JOIST_T_BLOCKED_RECV ||
             r->state == JOIST_T_BLOCKED_WAIT) &&
            r->ep != 0) {
            q_remove(k, i, r->ep - 1u);
        }
        /* A caller timing out in BLOCKED_REPLY leaves the server's reply
         * capability stale; its Reply then returns JOIST_E_PEER. */
        joist__wake(k, i, JOIST_E_TIMEOUT);
    }
}

/* ---- invariants ---- */

unsigned joist__check_ipc(const struct joist_kernel *k, char *msg, unsigned msglen, int msg_free)
{
    unsigned bad = 0;
    uint32_t seen_in_queue[JOIST_MAX_THREADS];
    memset(seen_in_queue, 0, sizeof seen_in_queue);

#define VIOLATION(...)                                                                             \
    do {                                                                                           \
        if (bad++ == 0 && msg_free && msg != NULL && msglen > 0) {                                 \
            JOIST_FMT(msg, msglen, __VA_ARGS__);                                              \
        }                                                                                          \
    } while (0)

    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *e = &k->obj[o];
        if (e->type != JOIST_OBJ_ENDPOINT && e->type != JOIST_OBJ_NOTIFICATION) {
            if (e->qkind != JOIST_Q_NONE || e->qhead != 0 || e->qtail != 0) {
                VIOLATION("IPC: object %u has a queue but takes no waiters", o);
            }
            continue;
        }
        if ((e->type == JOIST_OBJ_NOTIFICATION && e->qkind != JOIST_Q_NONE &&
             e->qkind != JOIST_Q_WAITERS) ||
            (e->type == JOIST_OBJ_ENDPOINT && e->qkind == JOIST_Q_WAITERS)) {
            VIOLATION("IPC: object %u type %u has queue kind %u", o, e->type, e->qkind);
        }
        if ((e->qhead == 0) != (e->qkind == JOIST_Q_NONE) || (e->qhead == 0) != (e->qtail == 0)) {
            VIOLATION("IPC: endpoint %u queue head/tail/kind inconsistent", o);
            continue;
        }
        uint32_t prev = 0;
        uint32_t cur = e->qhead;
        uint32_t steps = 0;
        uint8_t want = e->qkind == JOIST_Q_SENDERS     ? JOIST_T_BLOCKED_SEND
                       : e->qkind == JOIST_Q_RECEIVERS ? JOIST_T_BLOCKED_RECV
                                                       : JOIST_T_BLOCKED_WAIT;
        while (cur != 0) {
            if (cur > JOIST_MAX_THREADS || ++steps > JOIST_MAX_THREADS) {
                VIOLATION("IPC: endpoint %u queue is corrupt or cyclic", o);
                break;
            }
            const struct joist_thread_rec *r = &k->th[cur - 1u];
            if (r->state != want) {
                VIOLATION("IPC: thread %u in endpoint %u queue has state %u, queue kind %u",
                          cur - 1u, o, r->state, e->qkind);
            }
            if (r->ep != o + 1u || r->qprev != prev) {
                VIOLATION("IPC: thread %u links inconsistent in endpoint %u queue", cur - 1u, o);
            }
            seen_in_queue[cur - 1u]++;
            prev = cur;
            cur = r->qnext;
        }
        if (cur == 0 && prev != e->qtail) {
            VIOLATION("IPC: endpoint %u tail %u, last queued %u", o, e->qtail, prev);
        }
    }
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        const struct joist_thread_rec *r = &k->th[t];
        if (r->state == JOIST_T_FREE) {
            continue;
        }
        if (r->cs >= JOIST_MAX_CSPACES || !k->cs[r->cs].live) {
            VIOLATION("I1: thread %u is owned by dead space %u", t, r->cs);
        }
        int queued = r->state == JOIST_T_BLOCKED_SEND || r->state == JOIST_T_BLOCKED_RECV ||
                     r->state == JOIST_T_BLOCKED_WAIT;
        if (queued != (seen_in_queue[t] == 1) || seen_in_queue[t] > 1) {
            VIOLATION("IPC: thread %u state %u but found in %u queue(s)", t, r->state,
                      seen_in_queue[t]);
        }
        if ((r->state == JOIST_T_BLOCKED_RECV || r->state == JOIST_T_BLOCKED_WAIT) &&
            r->reply_to != 0) {
            VIOLATION("I10: thread %u waits while holding a reply capability", t);
        }
        /* A caller blocks, so it can never receive its own call (CBMC). */
        if (r->reply_to == t + 1u) {
            VIOLATION("I6: thread %u holds a reply capability naming itself", t);
        }
        if (r->reply_to > JOIST_MAX_THREADS) {
            VIOLATION("I6: thread %u holds a reply capability naming thread %u", t, r->reply_to);
        }
        if (r->state == JOIST_T_BLOCKED_REPLY) {
            /* I6/I10: exactly one valid reply capability names a waiting caller,
             * or it could wait forever with nobody able to answer. */
            uint32_t holders = 0;
            for (uint32_t s = 0; s < JOIST_MAX_THREADS; s++) {
                const struct joist_thread_rec *h = &k->th[s];
                if (h->state != JOIST_T_FREE && h->reply_to == t + 1u &&
                    h->reply_seq == r->call_seq) {
                    holders++;
                }
            }
            for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
                const struct joist_object *ro = &k->obj[o];
                if (ro->type == JOIST_OBJ_REPLY && ro->target == t + 1u &&
                    ro->rseq == r->call_seq) {
                    holders++;
                }
            }
            if (holders != 1) {
                VIOLATION("I6: caller %u waits for a reply but %u valid reply capabilities "
                          "name it",
                          t, holders);
            }
        }
    }
#undef VIOLATION
    return bad;
}
