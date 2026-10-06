/* notify.c - notifications, endpoint death, no-senders and dead-name
 * notifications (Phase 1 work item 3; invariants I2, I4, I5).
 *
 * Bounds (I9): signal and wait touch one object and one thread; accounting
 * lookups scan the fixed JOIST_MAX_BADGE_ACCTS table; endpoint death and a
 * notification's destruction scan the fixed dead-name table, the object table
 * and one queue. Nothing here allocates except joist__acct_reserve (at mint)
 * and joist_request_deadname, both from fixed tables, failing with
 * JOIST_E_NOMEM rather than blocking. */
#include "internal.h"

/* ---- accounting (no-senders) ---- */

/* The live (not fired) account for (obj, b). A fired account for the same
 * pair may also exist, waiting to be collected; a new mint of that badge
 * then starts a fresh account. */
static struct joist_badge_acct *acct_find(struct joist_kernel *k, uint32_t obj, joist_badge b)
{
    for (uint32_t i = 0; i < JOIST_MAX_BADGE_ACCTS; i++) {
        JOIST_WORK(k);
        if (k->acct[i].obj == obj + 1u && k->acct[i].badge == b && !k->acct[i].fired) {
            return &k->acct[i];
        }
    }
    return NULL;
}

joist_err joist__acct_reserve(struct joist_kernel *k, uint32_t obj, joist_badge badge)
{
    if (acct_find(k, obj, badge) != NULL) {
        return JOIST_OK;
    }
    for (uint32_t i = 0; i < JOIST_MAX_BADGE_ACCTS; i++) {
        JOIST_WORK(k);
        if (k->acct[i].obj == 0) {
            k->acct[i] = (struct joist_badge_acct){.obj = obj + 1u, .badge = badge, .send_caps = 0};
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

static int counted_send(const struct joist_cap *c)
{
    return c->type == JOIST_OBJ_ENDPOINT && (c->rights & JOIST_R_SEND) != 0 && c->badge != 0;
}

void joist__cap_added(struct joist_kernel *k, const struct joist_cap *c)
{
    uint32_t obj = c->obj - 1u;
    if (c->type != JOIST_OBJ_ENDPOINT) {
        return;
    }
    if ((c->rights & JOIST_R_RECV) != 0) {
        k->obj[obj].recv_caps++;
    }
    if (counted_send(c)) {
        /* The entry exists: mint reserved it, or the source capability of a
         * derive is itself counted under it. */
        struct joist_badge_acct *a = acct_find(k, obj, c->badge);
        if (a != NULL) {
            a->send_caps++;
        }
    }
}

static void endpoint_die(struct joist_kernel *k, uint32_t obj)
{
    struct joist_object *e = &k->obj[obj];
    e->dead = 1;
    joist__queue_wake_all(k, obj, JOIST_E_GONE);
    /* Queued no-senders badges: nobody can collect them now. */
    while (e->ns_first != 0) {
        JOIST_WORK(k);
        struct joist_badge_acct *a = &k->acct[e->ns_first - 1u];
        e->ns_first = a->next;
        memset(a, 0, sizeof *a);
    }
    e->ns_last = 0;
    /* I5: each registered holder is told once, then the registration goes. */
    for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
        JOIST_WORK(k);
        struct joist_deadname *d = &k->dn[i];
        if (d->ep == obj + 1u) {
            uint32_t notif = d->notif - 1u;
            uint64_t bits = d->bits;
            memset(d, 0, sizeof *d);
            joist__notify_signal(k, notif, bits);
        }
    }
}

void joist__cap_removed(struct joist_kernel *k, const struct joist_cap *c)
{
    uint32_t obj = c->obj - 1u;
    if (c->type != JOIST_OBJ_ENDPOINT) {
        return;
    }
    struct joist_object *e = &k->obj[obj];
    if (counted_send(c)) {
        struct joist_badge_acct *a = acct_find(k, obj, c->badge);
        if (a != NULL && --a->send_caps == 0) {
            /* I4: the last sender with this badge is gone. Queue the badge for
             * the owner, using the account itself as the queue node, and
             * signal the binding's summary bit. A dead or unbound endpoint
             * has nobody to tell, and the account is simply freed. */
            if (!e->dead && e->ns_notif != 0) {
                uint32_t idx = (uint32_t)(a - k->acct) + 1u;
                a->fired = 1;
                a->next = 0;
                if (e->ns_last != 0) {
                    k->acct[e->ns_last - 1u].next = idx;
                } else {
                    e->ns_first = idx;
                }
                e->ns_last = idx;
                joist__notify_signal(k, e->ns_notif - 1u, e->ns_bits);
            } else {
                memset(a, 0, sizeof *a);
            }
        }
    }
    if ((c->rights & JOIST_R_RECV) != 0 && --e->recv_caps == 0 && !e->dead) {
        endpoint_die(k, obj);
    }
}

void joist__object_freed(struct joist_kernel *k, uint32_t obj)
{
    struct joist_object *o = &k->obj[obj];
    joist__queue_wake_all(k, obj, JOIST_E_GONE);
    if (o->type == JOIST_OBJ_REPLY) {
        joist__reply_obj_freed(k, obj);
    }
    if (o->type == JOIST_OBJ_NOTIFICATION) {
        /* Drop every reference the kernel keeps to this notification. */
        for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
            JOIST_WORK(k);
            if (k->obj[i].ns_notif == obj + 1u) {
                k->obj[i].ns_notif = 0;
                k->obj[i].ns_bits = 0;
            }
        }
        for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
            JOIST_WORK(k);
            if (k->dn[i].notif == obj + 1u) {
                memset(&k->dn[i], 0, sizeof k->dn[i]);
            }
        }
    }
    if (o->type == JOIST_OBJ_ENDPOINT) {
        for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
            JOIST_WORK(k);
            if (k->dn[i].ep == obj + 1u) {
                memset(&k->dn[i], 0, sizeof k->dn[i]);
            }
        }
    }
}

/* ---- notifications ---- */

void joist__notify_signal(struct joist_kernel *k, uint32_t obj, uint64_t bits)
{
    struct joist_object *n = &k->obj[obj];
    if (n->qkind == JOIST_Q_WAITERS) {
        /* Waiters exist only while the word is empty, so the waiter gets
         * exactly these bits and nothing is left behind (I2). */
        uint32_t w = joist__q_pop(k, obj);
        k->th[w].badge = bits;
        joist__wake(k, w, JOIST_OK);
        return;
    }
    n->word |= bits;
}

static joist_err note_lookup(const struct joist_kernel *k, uint32_t cs, joist_slot slot,
                             joist_rights need, uint32_t *obj, joist_badge *badge)
{
    if (slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *c = &k->cs[cs].slot[slot];
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (c->type != JOIST_OBJ_NOTIFICATION) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & need) == 0) {
        return JOIST_E_RIGHTS;
    }
    *obj = c->obj - 1u;
    *badge = c->badge;
    return JOIST_OK;
}

static joist_err thread_running(const struct joist_kernel *k, joist_thread t)
{
    if (t >= JOIST_MAX_THREADS) {
        return JOIST_E_RANGE;
    }
    return k->th[t].state == JOIST_T_RUNNING ? JOIST_OK : JOIST_E_STATE;
}

joist_err joist_signal(struct joist_kernel *k, joist_thread t, joist_slot slot)
{
    uint32_t obj;
    joist_badge badge;
    joist_err e = thread_running(k, t);
    if (e != JOIST_OK) {
        return e;
    }
    e = note_lookup(k, k->th[t].cs, slot, JOIST_R_SEND, &obj, &badge);
    if (e != JOIST_OK) {
        return e;
    }
    if (badge == 0) {
        return JOIST_E_BADGE; /* an unbadged capability would signal nothing */
    }
    joist__notify_signal(k, obj, badge);
    return JOIST_OK;
}

joist_err joist_wait(struct joist_kernel *k, joist_thread t, joist_slot slot, uint64_t timeout)
{
    uint32_t obj;
    joist_badge badge;
    joist_err e = thread_running(k, t);
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_thread_rec *me = &k->th[t];
    /* As with Recv: blocking while holding a reply capability could strand
     * the caller (I10). */
    if (me->reply_to != 0) {
        return JOIST_E_STATE;
    }
    e = note_lookup(k, me->cs, slot, JOIST_R_RECV, &obj, &badge);
    if (e != JOIST_OK) {
        return e;
    }
    struct joist_object *n = &k->obj[obj];
    if (n->word != 0) {
        me->badge = n->word;
        n->word = 0; /* bits clear only when returned (I2) */
        me->result = JOIST_OK;
        return JOIST_OK;
    }
    if (timeout == 0) {
        return JOIST_E_TIMEOUT;
    }
    me->state = JOIST_T_BLOCKED_WAIT;
    me->deadline = joist__deadline(k, timeout);
    joist__q_push(k, obj, JOIST_Q_WAITERS, t);
    return JOIST_BLOCKED;
}

/* ---- registration ---- */

static joist_err space_live(const struct joist_kernel *k, joist_cspace cs)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    return k->cs[cs].live ? JOIST_OK : JOIST_E_DEAD;
}

joist_err joist_bind_nosenders(struct joist_kernel *k, joist_cspace cs, joist_slot ep_slot,
                               joist_slot note_slot)
{
    uint32_t nobj;
    joist_badge nbadge;
    joist_err e = space_live(k, cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (ep_slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *ec = &k->cs[cs].slot[ep_slot];
    if (ec->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (ec->type != JOIST_OBJ_ENDPOINT) {
        return JOIST_E_TYPE;
    }
    if ((ec->rights & JOIST_R_RECV) == 0) {
        return JOIST_E_RIGHTS; /* only the endpoint's owner binds */
    }
    e = note_lookup(k, cs, note_slot, JOIST_R_SEND, &nobj, &nbadge);
    if (e != JOIST_OK) {
        return e;
    }
    if (nbadge == 0) {
        return JOIST_E_BADGE; /* the signal would carry no bits */
    }
    k->obj[ec->obj - 1u].ns_notif = nobj + 1u;
    k->obj[ec->obj - 1u].ns_bits = nbadge;
    return JOIST_OK;
}

joist_err joist_nosenders_next(struct joist_kernel *k, joist_cspace cs, joist_slot ep_slot,
                               joist_badge *badge)
{
    joist_err e = space_live(k, cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (ep_slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *ec = &k->cs[cs].slot[ep_slot];
    if (ec->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (ec->type != JOIST_OBJ_ENDPOINT) {
        return JOIST_E_TYPE;
    }
    if ((ec->rights & JOIST_R_RECV) == 0) {
        return JOIST_E_RIGHTS; /* only the endpoint's owner collects */
    }
    struct joist_object *o = &k->obj[ec->obj - 1u];
    *badge = 0;
    if (o->ns_first != 0) {
        struct joist_badge_acct *a = &k->acct[o->ns_first - 1u];
        *badge = a->badge;
        o->ns_first = a->next;
        if (o->ns_first == 0) {
            o->ns_last = 0;
        }
        memset(a, 0, sizeof *a);
    }
    return JOIST_OK;
}

joist_err joist_request_deadname(struct joist_kernel *k, joist_cspace cs, joist_slot ep_slot,
                                 joist_slot note_slot)
{
    uint32_t nobj;
    joist_badge nbadge;
    joist_err e = space_live(k, cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (ep_slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *ec = &k->cs[cs].slot[ep_slot];
    if (ec->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (ec->type != JOIST_OBJ_ENDPOINT) {
        return JOIST_E_TYPE;
    }
    e = note_lookup(k, cs, note_slot, JOIST_R_SEND, &nobj, &nbadge);
    if (e != JOIST_OK) {
        return e;
    }
    if (nbadge == 0) {
        return JOIST_E_BADGE;
    }
    if (k->obj[ec->obj - 1u].dead) {
        return JOIST_E_GONE;
    }
    for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
        JOIST_WORK(k);
        if (k->dn[i].ep == 0) {
            k->dn[i] = (struct joist_deadname){.ep = ec->obj, .notif = nobj + 1u, .bits = nbadge};
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

/* ---- invariants ---- */

unsigned joist__check_notify(const struct joist_kernel *k, char *msg, unsigned msglen, int msg_free)
{
    unsigned bad = 0;
    uint32_t recv_seen[JOIST_MAX_OBJECTS];
    uint32_t fired_seen[JOIST_MAX_BADGE_ACCTS];
    memset(recv_seen, 0, sizeof recv_seen);
    memset(fired_seen, 0, sizeof fired_seen);

#define VIOLATION(...)                                                                             \
    do {                                                                                           \
        if (bad++ == 0 && msg_free && msg != NULL && msglen > 0) {                                 \
            JOIST_FMT(msg, msglen, __VA_ARGS__);                                              \
        }                                                                                          \
    } while (0)

    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
            const struct joist_cap *cap = &k->cs[c].slot[i];
            if (cap->obj == 0 || cap->obj > JOIST_MAX_OBJECTS) {
                continue;
            }
            if (cap->type == JOIST_OBJ_ENDPOINT && (cap->rights & JOIST_R_RECV) != 0) {
                recv_seen[cap->obj - 1u]++;
            }
            if (counted_send(cap)) {
                const struct joist_badge_acct *a = NULL;
                for (uint32_t j = 0; j < JOIST_MAX_BADGE_ACCTS; j++) {
                    if (k->acct[j].obj == cap->obj && k->acct[j].badge == cap->badge &&
                        !k->acct[j].fired) {
                        a = &k->acct[j];
                    }
                }
                if (a == NULL) {
                    VIOLATION("I4: space %u slot %u: badged send capability has no account", c, i);
                }
            }
        }
    }
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *ob = &k->obj[o];
        if (ob->type == JOIST_OBJ_ENDPOINT) {
            if (ob->recv_caps != recv_seen[o]) {
                VIOLATION("I5: endpoint %u recv_caps %u but %u RECV capabilities", o, ob->recv_caps,
                          recv_seen[o]);
            }
            if ((ob->dead != 0) != (ob->recv_caps == 0)) {
                VIOLATION("I5: endpoint %u dead=%u with %u RECV capabilities", o, ob->dead,
                          ob->recv_caps);
            }
            if (ob->dead && ob->qhead != 0) {
                VIOLATION("I5: dead endpoint %u still has a queue", o);
            }
            if (ob->word != 0) {
                VIOLATION("endpoint %u has a notification word", o);
            }
        } else if (ob->type == JOIST_OBJ_NOTIFICATION) {
            if (ob->qkind == JOIST_Q_WAITERS && ob->word != 0) {
                VIOLATION("I2: notification %u has waiters and pending bits 0x%llx", o,
                          (unsigned long long)ob->word);
            }
            if (ob->recv_caps != 0 || ob->dead) {
                VIOLATION("notification %u carries endpoint state", o);
            }
        } else if (ob->type == JOIST_OBJ_REPLY) {
            if (ob->word != 0 || ob->recv_caps != 0 || ob->dead) {
                VIOLATION("reply object %u carries other objects' state", o);
            }
            if (ob->target > JOIST_MAX_THREADS) {
                VIOLATION("I6: reply object %u names thread %u", o, ob->target);
            }
        } else if (ob->type == JOIST_OBJ_TASK || ob->type == JOIST_OBJ_THREAD) {
            if (ob->word != 0 || ob->recv_caps != 0) {
                VIOLATION("task/thread object %u carries endpoint or notification state", o);
            }
        } else if (ob->word != 0 || ob->recv_caps != 0 || ob->dead || ob->target != 0 ||
                   ob->rseq != 0 || ob->donor != 0 || ob->ns_notif != 0 || ob->ns_bits != 0 ||
                   ob->ns_first != 0 || ob->ns_last != 0 || ob->qkind != JOIST_Q_NONE ||
                   ob->qhead != 0 || ob->qtail != 0) {
            /* A free object is all zero: nothing survives into its next use. */
            VIOLATION("free object %u carries state", o);
        }
        if (ob->ns_notif != 0 &&
            (ob->type != JOIST_OBJ_ENDPOINT || ob->ns_notif > JOIST_MAX_OBJECTS ||
             k->obj[ob->ns_notif - 1u].type != JOIST_OBJ_NOTIFICATION)) {
            VIOLATION("I4: object %u no-senders binding %u is not a live notification", o,
                      ob->ns_notif);
        }
        if ((ob->ns_notif != 0) != (ob->ns_bits != 0)) {
            VIOLATION("I4: object %u binding %u with bits 0x%llx", o, ob->ns_notif,
                      (unsigned long long)ob->ns_bits);
        }
        /* The fired queue: only on a live endpoint, every node a fired
         * account of this endpoint, ending at ns_last, no cycle. */
        if (ob->ns_first != 0 && (ob->type != JOIST_OBJ_ENDPOINT || ob->dead)) {
            VIOLATION("I4: object %u (dead or not an endpoint) has queued no-senders", o);
        }
        uint32_t cur = ob->ns_first;
        uint32_t prev = 0;
        uint32_t steps = 0;
        while (cur != 0) {
            if (cur > JOIST_MAX_BADGE_ACCTS || ++steps > JOIST_MAX_BADGE_ACCTS ||
                !k->acct[cur - 1u].fired || k->acct[cur - 1u].obj != o + 1u) {
                VIOLATION("I4: endpoint %u no-senders queue is corrupt", o);
                break;
            }
            fired_seen[cur - 1u]++;
            prev = cur;
            cur = k->acct[cur - 1u].next;
        }
        if (cur == 0 && prev != ob->ns_last) {
            VIOLATION("I4: endpoint %u no-senders queue ends at %u, last %u", o, prev, ob->ns_last);
        }
    }
    for (uint32_t j = 0; j < JOIST_MAX_BADGE_ACCTS; j++) {
        const struct joist_badge_acct *a = &k->acct[j];
        if (a->obj == 0) {
            continue;
        }
        if (a->obj > JOIST_MAX_OBJECTS || k->obj[a->obj - 1u].type != JOIST_OBJ_ENDPOINT) {
            VIOLATION("I4: account %u names a non-endpoint %u", j, a->obj);
            continue;
        }
        if (a->fired) {
            /* Collected exactly once: queued on its endpoint exactly once. */
            if (a->send_caps != 0 || fired_seen[j] != 1) {
                VIOLATION("I4: fired account %u counts %u senders, queued %u times", j,
                          a->send_caps, fired_seen[j]);
            }
            continue;
        }
        if (a->next != 0) {
            VIOLATION("I4: live account %u has a queue link", j);
        }
        uint32_t n = 0;
        for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
            for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
                const struct joist_cap *cap = &k->cs[c].slot[i];
                if (cap->obj == a->obj && cap->badge == a->badge && counted_send(cap)) {
                    n++;
                }
            }
        }
        if (n != a->send_caps || n == 0) {
            VIOLATION("I4: account %u (endpoint %u badge %llu) counts %u, %u exist", j, a->obj - 1u,
                      (unsigned long long)a->badge, a->send_caps, n);
        }
        for (uint32_t j2 = j + 1u; j2 < JOIST_MAX_BADGE_ACCTS; j2++) {
            if (k->acct[j2].obj == a->obj && k->acct[j2].badge == a->badge && !k->acct[j2].fired) {
                VIOLATION("I4: duplicate accounts %u and %u", j, j2);
            }
        }
    }
    for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
        const struct joist_deadname *d = &k->dn[i];
        if (d->ep == 0) {
            continue;
        }
        if (d->ep > JOIST_MAX_OBJECTS || k->obj[d->ep - 1u].type != JOIST_OBJ_ENDPOINT ||
            k->obj[d->ep - 1u].dead) {
            VIOLATION("I5: dead-name registration %u names a dead or freed endpoint", i);
        }
        if (d->notif == 0 || d->notif > JOIST_MAX_OBJECTS ||
            k->obj[d->notif - 1u].type != JOIST_OBJ_NOTIFICATION) {
            VIOLATION("I5: dead-name registration %u names a freed notification", i);
        }
    }
#undef VIOLATION
    return bad;
}
