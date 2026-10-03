/* exec.c - the operation-sequence executor (Phase 1 work item 5). See exec.h.
 *
 * Choices are biased towards operations that can succeed (live spaces,
 * occupied slots of the right type, running threads, threads holding reply
 * capabilities), because a uniform choice spends almost every step on
 * argument errors. Out-of-range values stay possible for every argument. */
#include "exec.h"
#include "internal.h"

#include <stdio.h>
#include <string.h>

const char *const jx_op_name[JX_OPS] = {
    "space_create", "space_destroy", "obj_create",   "derive",   "mint",  "grant",
    "delete",       "task_create",   "task_destroy", "spawn",    "kill",  "th_create",
    "th_destroy",   "send",          "call",         "recv",     "reply", "reply_recv",
    "signal",       "wait",          "bind",         "deadname", "tick"};

/* Relative frequency of each operation, out of their sum. */
static const uint8_t weight[JX_OPS] = {
    [JX_SPACE_CREATE] = 1, [JX_SPACE_DESTROY] = 1, [JX_OBJ_CREATE] = 10, [JX_DERIVE] = 6,
    [JX_MINT] = 6,         [JX_GRANT] = 10,        [JX_DELETE] = 8,      [JX_TASK_CREATE] = 5,
    [JX_TASK_DESTROY] = 2, [JX_SPAWN] = 5,         [JX_KILL] = 2,        [JX_TH_CREATE] = 2,
    [JX_TH_DESTROY] = 1,   [JX_SEND] = 5,          [JX_CALL] = 6,        [JX_RECV] = 6,
    [JX_REPLY] = 4,        [JX_REPLY_RECV] = 4,    [JX_SIGNAL] = 5,      [JX_WAIT] = 5,
    [JX_BIND] = 3,         [JX_DEADNAME] = 3,      [JX_TICK] = 6};

static uint64_t boot_budget;

int jx_op_cascades(unsigned op)
{
    return op == JX_SPACE_DESTROY || op == JX_DELETE || op == JX_TASK_DESTROY || op == JX_KILL ||
           op == JX_TH_DESTROY;
}

static uint32_t ch(struct jx_source *src, uint32_t n)
{
    return src->next(src->ctx, n);
}

/* Pick uniformly among the indexes in [0, n) for which ok() holds, with
 * probability (bias - 1) / bias when there is one; otherwise any index, one
 * past the end included. (Taking the first match after a random start would
 * favour the first of every run of matches.) */
static uint32_t pick_where(struct jx_source *src, uint32_t n, uint32_t bias,
                           int (*ok)(const struct joist_kernel *, uint32_t, const void *),
                           const struct joist_kernel *k, const void *arg)
{
    if (ch(src, bias) != 0) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < n; i++) {
            count += (uint32_t)(ok(k, i, arg) != 0);
        }
        if (count != 0) {
            uint32_t r = ch(src, count);
            for (uint32_t i = 0; i < n; i++) {
                if (ok(k, i, arg) && r-- == 0) {
                    return i;
                }
            }
        }
    }
    return ch(src, n + 1u);
}

static int space_ok(const struct joist_kernel *k, uint32_t i, const void *arg)
{
    (void)arg;
    return k->cs[i].live;
}

struct slot_want {
    uint32_t cs;
    uint8_t type; /* JOIST_OBJ_NONE: any occupied slot */
    int free;     /* want an empty slot instead */
};

static int slot_ok(const struct joist_kernel *k, uint32_t i, const void *arg)
{
    const struct slot_want *w = arg;
    const struct joist_cap *c = &k->cs[w->cs].slot[i];
    if (w->free) {
        return c->obj == 0;
    }
    return c->obj != 0 && (w->type == JOIST_OBJ_NONE || c->type == w->type);
}

static int thread_ok(const struct joist_kernel *k, uint32_t i, const void *arg)
{
    const int *want_reply = arg;
    return k->th[i].state == JOIST_T_RUNNING && (!*want_reply || k->th[i].reply_to != 0);
}

/* A live space 7 times in 8. */
static uint32_t pick_space(const struct joist_kernel *k, struct jx_source *src)
{
    return pick_where(src, JOIST_MAX_CSPACES, 8, space_ok, k, NULL);
}

/* An occupied slot of the given type (any type for NONE) 3 times in 4. */
static uint32_t pick_slot(const struct joist_kernel *k, struct jx_source *src, uint32_t cs,
                          uint8_t type)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return ch(src, JOIST_CSPACE_SLOTS + 1u);
    }
    struct slot_want w = {cs, type, 0};
    return pick_where(src, JOIST_CSPACE_SLOTS, 4, slot_ok, k, &w);
}

/* A free slot 3 times in 4. */
static uint32_t pick_dst(const struct joist_kernel *k, struct jx_source *src, uint32_t cs)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return ch(src, JOIST_CSPACE_SLOTS + 1u);
    }
    struct slot_want w = {cs, JOIST_OBJ_NONE, 1};
    return pick_where(src, JOIST_CSPACE_SLOTS, 4, slot_ok, k, &w);
}

/* A running thread (holding a reply capability, if want_reply) 7 times in 8. */
static uint32_t pick_thread(const struct joist_kernel *k, struct jx_source *src, int want_reply)
{
    return pick_where(src, JOIST_MAX_THREADS, 8, thread_ok, k, &want_reply);
}

/* The space a thread runs in, or an out-of-range index for a bad thread. */
static uint32_t thread_space(const struct joist_kernel *k, uint32_t t)
{
    return (t < JOIST_MAX_THREADS && k->th[t].state != JOIST_T_FREE) ? k->th[t].cs
                                                                     : JOIST_MAX_CSPACES;
}

static uint64_t pick_timeout(struct jx_source *src)
{
    switch (ch(src, 4)) {
    case 0:
        return 0;
    case 1:
        return JOIST_FOREVER;
    default:
        return 1u + ch(src, 5);
    }
}

/* Badges: mostly single bits (so no-senders bits are distinguishable), some
 * arbitrary values, and 0 (refused by mint) now and then. */
static joist_badge pick_badge(struct jx_source *src)
{
    uint32_t r = ch(src, 8);
    if (r == 0) {
        return 0;
    }
    if (r == 1) {
        return ((joist_badge)ch(src, 0xffffu) << 16) | 1u;
    }
    return (joist_badge)1u << ch(src, 16);
}

void jx_begin(struct joist_kernel *k, struct jx_source *src)
{
    joist_cspace root;
    joist_init(k);
    boot_budget = 8u + ch(src, 120);
    (void)joist_boot(k, (uint32_t)boot_budget, &root);
    /* A starting world, so every sequence can reach IPC at once instead of
     * waiting for random choices to build one: an endpoint and a send-only
     * copy, a notification and two copies with different badges (so signals
     * can pile up), three threads. */
    joist_thread t;
    (void)joist_obj_create(k, root, JOIST_OBJ_ENDPOINT, 0);
    (void)joist_cap_derive(k, root, 0, 1, JOIST_R_SEND);
    (void)joist_obj_create(k, root, JOIST_OBJ_NOTIFICATION, 2);
    (void)joist_cap_mint(k, root, 2, 3, JOIST_R_ALL, 1);
    (void)joist_cap_mint(k, root, 2, 4, JOIST_R_ALL, 2);
    for (int i = 0; i < 3; i++) {
        (void)joist_thread_create(k, root, &t);
    }
}

/* What a step did, for the checks that need more than the final state. */
struct last {
    uint32_t t;       /* the acting thread */
    uint32_t obj;     /* SIGNAL: the notification object */
    joist_badge bits; /* SIGNAL: the badge signalled */
    uint64_t word;    /* SIGNAL: the notification's pending word before */
    uint32_t caller;  /* REPLY: the caller the reply capability named, plus one */
};

static joist_err apply(struct joist_kernel *k, struct jx_source *src, unsigned op,
                       unsigned long step, struct last *ls, int *bad, char *why, unsigned whylen)
{
    uint32_t cs = pick_space(k, src);
    struct joist_msg m = {{0}};
    m.r[0] = step;
    switch (op) {
    case JX_SPACE_CREATE: {
        joist_cspace out;
        return joist_cspace_create(k, ch(src, 6), &out);
    }
    case JX_SPACE_DESTROY:
        return joist_cspace_destroy(k, cs);
    case JX_OBJ_CREATE:
        /* An invalid type (none, task, thread) 1 time in 8. */
        return joist_obj_create(k, cs,
                                (joist_obj_type)(ch(src, 8) == 0 ? ch(src, JOIST_OBJ_THREAD + 1u)
                                                                 : JOIST_OBJ_ENDPOINT + ch(src, 2)),
                                pick_dst(k, src, cs));
    case JX_DERIVE:
    case JX_MINT:
    case JX_GRANT: {
        uint32_t s = pick_slot(k, src, cs, JOIST_OBJ_NONE);
        uint32_t to = op == JX_GRANT ? pick_space(k, src) : cs;
        uint32_t d = pick_dst(k, src, to);
        joist_rights mask = ch(src, 3) == 0 ? (joist_rights)ch(src, 16) : JOIST_R_ALL;
        joist_err e = op == JX_DERIVE ? joist_cap_derive(k, cs, s, d, mask)
                      : op == JX_MINT ? joist_cap_mint(k, cs, s, d, mask, pick_badge(src))
                                      : joist__cap_grant(k, cs, s, to, d, mask);
        /* I8, directly: the copy never has a right its source lacks. */
        if (e == JOIST_OK &&
            (k->cs[to].slot[d].rights & (joist_rights)~k->cs[cs].slot[s].rights) != 0) {
            (void)snprintf(why, whylen, "I8: %s grew rights 0x%x -> 0x%x", jx_op_name[op],
                           k->cs[cs].slot[s].rights, k->cs[to].slot[d].rights);
            *bad = 1;
        }
        return e;
    }
    case JX_DELETE:
        return joist_cap_delete(k, cs, pick_slot(k, src, cs, JOIST_OBJ_NONE));
    case JX_TASK_CREATE:
        return joist_task_create(k, cs, ch(src, 16) == 0 ? UINT32_MAX : ch(src, 10),
                                 pick_dst(k, src, cs));
    case JX_TASK_DESTROY:
        return joist_task_destroy(k, cs, pick_slot(k, src, cs, JOIST_OBJ_TASK));
    case JX_SPAWN: {
        joist_thread out;
        uint32_t ts = pick_slot(k, src, cs, JOIST_OBJ_TASK);
        return joist_thread_spawn(k, cs, ts, pick_dst(k, src, cs), &out);
    }
    case JX_KILL:
        return joist_thread_kill(k, cs, pick_slot(k, src, cs, JOIST_OBJ_THREAD));
    case JX_TH_CREATE: {
        joist_thread out;
        return joist_thread_create(k, cs, &out);
    }
    case JX_TH_DESTROY:
        return joist_thread_destroy(k, ch(src, JOIST_MAX_THREADS + 1u));
    case JX_SEND:
    case JX_CALL:
    case JX_RECV: {
        uint32_t t = pick_thread(k, src, 0);
        uint32_t ep = pick_slot(k, src, thread_space(k, t), JOIST_OBJ_ENDPOINT);
        uint64_t to = pick_timeout(src);
        return op == JX_SEND   ? joist_send(k, t, ep, &m, to)
               : op == JX_CALL ? joist_call(k, t, ep, &m, to)
                               : joist_recv(k, t, ep, to);
    }
    case JX_REPLY:
        ls->t = pick_thread(k, src, 1);
        ls->caller = ls->t < JOIST_MAX_THREADS ? k->th[ls->t].reply_to : 0;
        return joist_reply(k, ls->t, &m);
    case JX_REPLY_RECV: {
        uint32_t t = pick_thread(k, src, ch(src, 2) != 0);
        uint32_t ep = pick_slot(k, src, thread_space(k, t), JOIST_OBJ_ENDPOINT);
        return joist_reply_recv(k, t, &m, ep, pick_timeout(src));
    }
    case JX_SIGNAL:
    case JX_WAIT: {
        uint32_t t = pick_thread(k, src, 0);
        uint32_t n = pick_slot(k, src, thread_space(k, t), JOIST_OBJ_NOTIFICATION);
        if (t < JOIST_MAX_THREADS && n < JOIST_CSPACE_SLOTS && k->th[t].state != JOIST_T_FREE) {
            const struct joist_cap *c = &k->cs[k->th[t].cs].slot[n];
            ls->obj = c->obj != 0 ? c->obj - 1u : 0;
            ls->bits = c->badge;
            ls->word = c->obj != 0 ? k->obj[c->obj - 1u].word : 0;
        }
        return op == JX_SIGNAL ? joist_signal(k, t, n) : joist_wait(k, t, n, pick_timeout(src));
    }
    case JX_BIND:
    case JX_DEADNAME: {
        uint32_t ep = pick_slot(k, src, cs, JOIST_OBJ_ENDPOINT);
        uint32_t n = pick_slot(k, src, cs, JOIST_OBJ_NOTIFICATION);
        return op == JX_BIND ? joist_bind_nosenders(k, cs, ep, n)
                             : joist_request_deadname(k, cs, ep, n);
    }
    default:
        joist_tick(k, 1u + ch(src, 6));
        return JOIST_OK;
    }
}

static unsigned bucket(joist_err e)
{
    return (unsigned)e < JX_RESULTS - 1u ? (unsigned)e : JX_RESULTS - 1u;
}

int jx_step(struct joist_kernel *k, struct jx_source *src, struct jx_stats *st, char *why,
            unsigned whylen)
{
    unsigned total = 0;
    for (unsigned i = 0; i < JX_OPS; i++) {
        total += weight[i];
    }
    uint32_t roll = ch(src, total);
    unsigned op = 0;
    while (roll >= weight[op]) {
        roll -= weight[op++];
    }

    /* Remember each thread's state, to see how each wake-up ended. */
    uint8_t before[JOIST_MAX_THREADS];
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        before[t] = k->th[t].state;
    }
    struct last ls = {0};

    int bad = 0;
    uint64_t w0 = k->work;
    why[0] = '\0';
    joist_err e = apply(k, src, op, st->steps, &ls, &bad, why, whylen);
    uint64_t w = k->work - w0;
    st->steps++;
    if (bad) {
        return 0;
    }
    st->result[op][bucket(e)]++;
    if (w > st->max_work[op]) {
        st->max_work[op] = w;
    }
    int delivered = 0; /* SIGNAL: a waiter woke with every signalled bit */
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        if (before[t] >= JOIST_T_BLOCKED_SEND && k->th[t].state == JOIST_T_RUNNING) {
            st->wakeups[bucket(k->th[t].result)]++;
            delivered |= before[t] == JOIST_T_BLOCKED_WAIT && k->th[t].result == JOIST_OK &&
                         (k->th[t].badge & ls.bits) == ls.bits;
        }
    }
    /* I2, directly: a signal's bits are pending, or a waiter received them,
     * and no bit that was already pending is lost. */
    uint64_t want = ls.word | ls.bits;
    if (op == JX_SIGNAL && e == JOIST_OK && !delivered && (k->obj[ls.obj].word & want) != want) {
        (void)snprintf(why, whylen, "I2: signal of 0x%llx to notification %u was lost",
                       (unsigned long long)ls.bits, ls.obj);
        return 0;
    }
    /* I6, directly: a reply consumes the reply capability and reaches the
     * caller, with this step's message. */
    if (op == JX_REPLY && e == JOIST_OK &&
        (k->th[ls.t].reply_to != 0 || ls.caller == 0 ||
         k->th[ls.caller - 1u].state != JOIST_T_RUNNING ||
         k->th[ls.caller - 1u].result != JOIST_OK ||
         k->th[ls.caller - 1u].msg.r[0] != st->steps - 1u)) {
        (void)snprintf(why, whylen,
                       "I6: reply by thread %u kept its capability or missed caller %u", ls.t,
                       ls.caller);
        return 0;
    }
    /* I9: one operation's work stays under its closed-form bound (exec.h). */
    uint64_t bound = jx_op_cascades(op) ? JX_WORK_CASCADE : JX_WORK_SIMPLE;
    if (w > bound) {
        (void)snprintf(why, whylen, "I9: %s took %llu units of work (bound %llu)", jx_op_name[op],
                       (unsigned long long)w, (unsigned long long)bound);
        return 0;
    }
    unsigned n = joist_check_invariants(k, why, whylen);
    if (n != 0) {
        size_t len = strlen(why);
        (void)snprintf(why + len, whylen - len, " (after %s -> %s)", jx_op_name[op],
                       joist_err_name(e));
        return 0;
    }
    return 1;
}

int jx_teardown(struct joist_kernel *k, struct jx_stats *st, char *why, unsigned whylen)
{
    uint32_t root = k->root - 1u;
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        if (k->cs[s].live && k->cs[s].fixture) {
            (void)joist_cspace_destroy(k, s);
        }
    }
    for (joist_slot i = 0; i < JOIST_CSPACE_SLOTS; i++) {
        if (k->cs[root].slot[i].obj != 0) {
            (void)joist_cap_delete(k, root, i);
        }
    }
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        if (s != root && k->cs[s].live) {
            st->cycles++;
            (void)joist_cspace_destroy(k, s);
        }
    }
    /* I10: with every object gone, nothing can be blocked on one. The only
     * wait left is a caller whose server, a live thread, still owes a reply. */
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        const struct joist_thread_rec *r = &k->th[t];
        if (r->state <= JOIST_T_RUNNING) {
            continue;
        }
        int owed = 0;
        for (uint32_t h = 0; r->state == JOIST_T_BLOCKED_REPLY && h < JOIST_MAX_THREADS; h++) {
            owed |= k->th[h].state != JOIST_T_FREE && k->th[h].reply_to == t + 1u &&
                    k->th[h].reply_seq == r->call_seq;
        }
        if (!owed) {
            (void)snprintf(why, whylen, "I10: thread %u still blocked (state %u) at teardown", t,
                           r->state);
            return 0;
        }
    }
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        if (k->th[t].state != JOIST_T_FREE) {
            (void)joist_thread_destroy(k, t);
        }
    }
    if (joist_check_invariants(k, why, whylen) != 0) {
        return 0;
    }
    uint32_t accts = 0;
    uint32_t dns = 0;
    for (uint32_t i = 0; i < JOIST_MAX_BADGE_ACCTS; i++) {
        accts += k->acct[i].obj != 0;
    }
    for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
        dns += k->dn[i].ep != 0;
    }
    uint32_t spaces = 0;
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        spaces += k->cs[s].live != 0;
    }
    if (k->objects_live != 0 || accts != 0 || dns != 0 || spaces != 1 ||
        k->cs[root].quota != boot_budget || k->budget != boot_budget) {
        (void)snprintf(why, whylen,
                       "I1/I3: teardown left %u objects, %u accounts, %u registrations, %u "
                       "spaces; root quota %u, budget %llu, booted with %llu",
                       k->objects_live, accts, dns, spaces, k->cs[root].quota,
                       (unsigned long long)k->budget, (unsigned long long)boot_budget);
        return 0;
    }
    return 1;
}
