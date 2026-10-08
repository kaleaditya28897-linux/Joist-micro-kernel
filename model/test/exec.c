/* exec.c - the operation-sequence executor (Phase 1 work item 5). See exec.h.
 *
 * Choices are biased towards operations that can succeed (live spaces,
 * occupied slots of the right type, running threads, threads holding reply
 * capabilities), because a uniform choice spends almost every step on
 * argument errors. Out-of-range values stay possible for every argument. */
#include "exec.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Designated, so an operation added to the enum without a name or weight is
 * caught by the checks in jx_begin rather than printing NULL. */
const char *const jx_op_name[JX_OPS] = {[JX_SPACE_CREATE] = "space_create",
                                        [JX_SPACE_DESTROY] = "space_destroy",
                                        [JX_OBJ_CREATE] = "obj_create",
                                        [JX_DERIVE] = "derive",
                                        [JX_MINT] = "mint",
                                        [JX_GRANT] = "grant",
                                        [JX_DELETE] = "delete",
                                        [JX_TASK_CREATE] = "task_create",
                                        [JX_TASK_DESTROY] = "task_destroy",
                                        [JX_SPAWN] = "spawn",
                                        [JX_KILL] = "kill",
                                        [JX_TH_CREATE] = "th_create",
                                        [JX_TH_DESTROY] = "th_destroy",

                                        [JX_CALL] = "call",
                                        [JX_RECV] = "recv",
                                        [JX_REPLY] = "reply",
                                        [JX_REPLY_RECV] = "reply_recv",
                                        [JX_SIGNAL] = "signal",
                                        [JX_WAIT] = "wait",
                                        [JX_BIND] = "bind",
                                        [JX_DEADNAME] = "deadname",
                                        [JX_COLLECT] = "collect",
                                        [JX_REAP] = "reap",
                                        [JX_DESCENDANT] = "descendant",
                                        [JX_SAVE] = "reply_save",
                                        [JX_SAVED] = "reply_saved",
                                        [JX_GROW] = "grow",
                                        [JX_SHRINK] = "shrink",
                                        [JX_RECV_SLOTS] = "recv_slots",
                                        [JX_TICK] = "tick"};

/* Relative frequency of each operation, out of their sum. */
static const uint8_t weight[JX_OPS] = {
    [JX_SPACE_CREATE] = 1, [JX_SPACE_DESTROY] = 1, [JX_OBJ_CREATE] = 10, [JX_DERIVE] = 6,
    [JX_MINT] = 6,         [JX_GRANT] = 10,        [JX_DELETE] = 8,      [JX_TASK_CREATE] = 5,
    [JX_TASK_DESTROY] = 2, [JX_SPAWN] = 5,         [JX_KILL] = 2,        [JX_TH_CREATE] = 2,
    [JX_TH_DESTROY] = 1,   [JX_CALL] = 11,         [JX_RECV] = 6,        [JX_REPLY] = 4,
    [JX_REPLY_RECV] = 4,   [JX_SIGNAL] = 5,        [JX_WAIT] = 5,        [JX_BIND] = 3,
    [JX_DEADNAME] = 3,     [JX_COLLECT] = 3,       [JX_REAP] = 2,        [JX_DESCENDANT] = 2,
    [JX_SAVE] = 3,         [JX_SAVED] = 3,         [JX_GROW] = 2,        [JX_SHRINK] = 2,
    [JX_RECV_SLOTS] = 3,
    [JX_TICK] = 6};

static uint64_t boot_budget;

int jx_op_cascades(unsigned op)
{
    return op == JX_SPACE_DESTROY || op == JX_DELETE || op == JX_TASK_DESTROY || op == JX_KILL ||
           op == JX_TH_DESTROY || op == JX_REAP;
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
    const struct joist_cap *c = joist__slot_c(k, w->cs, i);
    if (c == NULL) {
        return 0; /* its leaf is absent: neither free nor occupied */
    }
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

/* Half the time, attach capabilities from space cs to m: 0 to
 * JOIST_MSG_CAPS + 1 of them (one too many now and then), mostly occupied
 * slots, all rights or a random mask (capability transfer, Phase 3). */
static void pick_caps(const struct joist_kernel *k, struct jx_source *src, struct joist_msg *m,
                      uint32_t cs)
{
    if (ch(src, 2) == 0) {
        return;
    }
    m->ncaps = ch(src, JOIST_MSG_CAPS + 2u);
    for (uint32_t i = 0; i < JOIST_MSG_CAPS; i++) {
        m->cap[i] = pick_slot(k, src, cs, JOIST_OBJ_NONE);
        m->mask[i] = ch(src, 3) == 0 ? (joist_rights)ch(src, 16) : JOIST_R_ALL;
    }
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
    for (unsigned op = 0; op < JX_OPS; op++) {
        if (jx_op_name[op] == NULL || weight[op] == 0) {
            fprintf(stderr, "exec.c: operation %u has no name or weight\n", op);
            abort();
        }
    }
    joist_init(k);
    boot_budget = 8u + ch(src, 120);
    (void)joist_boot(k, (uint32_t)boot_budget, &root);
    /* A starting world, so every sequence can reach IPC at once instead of
     * waiting for random choices to build one: an endpoint and a send-only
     * copy, a notification and two copies with different badges (so signals
     * can pile up), a reply object, three threads. */
    joist_thread t;
    (void)joist_obj_create(k, root, JOIST_OBJ_ENDPOINT, 0);
    (void)joist_cap_derive(k, root, 0, 1, JOIST_R_SEND);
    (void)joist_obj_create(k, root, JOIST_OBJ_NOTIFICATION, 2);
    (void)joist_cap_mint(k, root, 2, 3, JOIST_R_ALL, 1);
    (void)joist_cap_mint(k, root, 2, 4, JOIST_R_ALL, 2);
    (void)joist_obj_create(k, root, JOIST_OBJ_REPLY, 5);
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
    struct joist_msg m = {0};
    m.r[0] = step;
    switch (op) {
    case JX_SPACE_CREATE: {
        joist_cspace out;
        return joist_cspace_create(k, ch(src, 6), &out);
    }
    case JX_SPACE_DESTROY:
        return joist_cspace_destroy(k, cs);
    case JX_OBJ_CREATE: {
        /* An invalid type (none, task, thread) 1 time in 8. */
        static const joist_obj_type valid[] = {JOIST_OBJ_ENDPOINT, JOIST_OBJ_NOTIFICATION,
                                               JOIST_OBJ_REPLY};
        joist_obj_type ty =
            ch(src, 8) == 0 ? (joist_obj_type)ch(src, JOIST_OBJ_THREAD + 1u) : valid[ch(src, 3)];
        return joist_obj_create(k, cs, ty, pick_dst(k, src, cs));
    }
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
        if (e == JOIST_OK) {
            const struct joist_cap *sc = joist__slot_c(k, cs, s);
            const struct joist_cap *dc = joist__slot_c(k, to, d);
            if ((dc->rights & (joist_rights)~sc->rights) != 0) {
                (void)snprintf(why, whylen, "I8: %s grew rights 0x%x -> 0x%x", jx_op_name[op],
                               sc->rights, dc->rights);
                *bad = 1;
            }
        }
        return e;
    }
    case JX_DELETE:
        return joist_cap_delete(k, cs, pick_slot(k, src, cs, JOIST_OBJ_NONE));
    case JX_TASK_CREATE:
        return joist_task_create(k, cs, ch(src, 16) == 0 ? UINT32_MAX : ch(src, 10),
                                 pick_dst(k, src, cs));
    case JX_REAP:
        /* From the root half the time, so ancestry usually holds. */
        return joist_task_reap(k, ch(src, 2) == 0 ? k->root - 1u : cs, pick_space(k, src));
    case JX_DESCENDANT: {
        joist_cspace out;
        return joist_task_descendant(k, cs, ch(src, JOIST_MAX_CSPACES + 2u), &out);
    }
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
    case JX_CALL:
    case JX_RECV: {
        uint32_t t = pick_thread(k, src, 0);
        uint32_t ep = pick_slot(k, src, thread_space(k, t), JOIST_OBJ_ENDPOINT);
        uint64_t to = pick_timeout(src);
        if (op == JX_CALL) {
            pick_caps(k, src, &m, thread_space(k, t));
        }
        return op == JX_CALL ? joist_call(k, t, ep, &m, to) : joist_recv(k, t, ep, to);
    }
    case JX_REPLY:
        ls->t = pick_thread(k, src, 1);
        ls->caller = ls->t < JOIST_MAX_THREADS ? k->th[ls->t].reply_to : 0;
        pick_caps(k, src, &m, thread_space(k, ls->t));
        return joist_reply(k, ls->t, &m);
    case JX_REPLY_RECV: {
        uint32_t t = pick_thread(k, src, ch(src, 2) != 0);
        uint32_t ep = pick_slot(k, src, thread_space(k, t), JOIST_OBJ_ENDPOINT);
        pick_caps(k, src, &m, thread_space(k, t));
        return joist_reply_recv(k, t, &m, ep, pick_timeout(src));
    }
    case JX_SIGNAL:
    case JX_WAIT: {
        uint32_t t = pick_thread(k, src, 0);
        uint32_t n = pick_slot(k, src, thread_space(k, t), JOIST_OBJ_NOTIFICATION);
        const struct joist_cap *c = t < JOIST_MAX_THREADS && k->th[t].state != JOIST_T_FREE
                                        ? joist__slot_c(k, k->th[t].cs, n)
                                        : NULL;
        if (c != NULL) {
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
    case JX_SAVE:
    case JX_SAVED: {
        uint32_t t = pick_thread(k, src, ch(src, 4) != 0);
        uint32_t r = pick_slot(k, src, thread_space(k, t), JOIST_OBJ_REPLY);
        if (op == JX_SAVE) {
            return joist_reply_save(k, t, r);
        }
        const struct joist_cap *c = t < JOIST_MAX_THREADS && k->th[t].state != JOIST_T_FREE
                                        ? joist__slot_c(k, k->th[t].cs, r)
                                        : NULL;
        if (c != NULL) {
            if (c->obj != 0 && c->type == JOIST_OBJ_REPLY) {
                ls->caller = k->obj[c->obj - 1u].target;
            }
        }
        ls->t = t;
        pick_caps(k, src, &m, thread_space(k, t));
        return joist_reply_saved(k, t, r, &m);
    }
    case JX_COLLECT: {
        joist_badge b;
        return joist_nosenders_next(k, cs, pick_slot(k, src, cs, JOIST_OBJ_ENDPOINT), &b);
    }
    case JX_RECV_SLOTS: {
        uint32_t t = pick_thread(k, src, 0);
        joist_slot s[JOIST_MSG_CAPS];
        for (uint32_t i = 0; i < JOIST_MSG_CAPS; i++) {
            s[i] = pick_dst(k, src, thread_space(k, t));
        }
        return joist_recv_slots(k, t, s, ch(src, JOIST_MSG_CAPS + 2u));
    }
    case JX_GROW:
    case JX_SHRINK: {
        /* A real leaf index (1 .. root - 1) 3 times in 4; else any, 0 and
         * one past the end included. */
        uint32_t l = JOIST_CNODE_ROOT > 1u && ch(src, 4) != 0 ? 1u + ch(src, JOIST_CNODE_ROOT - 1u)
                                                                : ch(src, JOIST_CNODE_ROOT + 1u);
        return op == JX_GROW ? joist_cspace_grow(k, cs, l) : joist_cspace_shrink(k, cs, l);
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
    static struct joist_msg msg_before[JOIST_MAX_THREADS];
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        before[t] = k->th[t].state;
        msg_before[t] = k->th[t].msg;
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
    /* Capability transfer coverage: positions of newly delivered messages. */
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        const struct joist_msg *m = &k->th[t].msg;
        if (memcmp(m, &msg_before[t], sizeof *m) != 0) {
            for (uint32_t i = 0; i < m->ncaps && i < JOIST_MSG_CAPS; i++) {
                if (m->cap[i] != JOIST_SLOT_NONE) {
                    st->caps_moved++;
                } else {
                    st->caps_skipped++;
                }
            }
        }
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
    if (op == JX_SAVED && e == JOIST_OK &&
        (ls.caller == 0 || k->th[ls.caller - 1u].state != JOIST_T_RUNNING ||
         k->th[ls.caller - 1u].result != JOIST_OK ||
         k->th[ls.caller - 1u].msg.r[0] != st->steps - 1u)) {
        (void)snprintf(why, whylen, "I6: saved reply by thread %u missed caller %u", ls.t,
                       ls.caller);
        return 0;
    }
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
    joist_cspace d;
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        if (k->cs[s].live && k->cs[s].fixture) {
            /* A fixture has no parent, so its children would be left with
             * none and nobody could reap them: reap them first. (Only test
             * fixtures lack a parent; every real task descends from the
             * root, which can't be destroyed.) */
            while (joist_task_descendant(k, s, 0, &d) == JOIST_OK) {
                if (joist_task_reap(k, s, d) != JOIST_OK) {
                    (void)snprintf(why, whylen, "teardown: fixture %u could not reap %u", s, d);
                    return 0;
                }
            }
            (void)joist_cspace_destroy(k, s);
        }
    }
    for (joist_slot i = 0; i < JOIST_CSPACE_SLOTS; i++) {
        const struct joist_cap *c = joist__slot_c(k, root, i);
        if (c != NULL && c->obj != 0) {
            (void)joist_cap_delete(k, root, i);
        }
    }
    /* The root's own extra leaves, now empty, go back too (one unit each). */
    for (uint32_t l = 1; l < JOIST_CNODE_ROOT; l++) {
        (void)joist_cspace_shrink(k, root, l);
    }
    /* What is left lives only in cycles. Reclaim it as a parent would
     * (0004 amendment): find it from the root, reap it. */
    while (joist_task_descendant(k, root, 0, &d) == JOIST_OK) {
        st->cycles++;
        if (joist_task_reap(k, root, d) != JOIST_OK) {
            (void)snprintf(why, whylen, "teardown: the root could not reap task %u", d);
            return 0;
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
        k->cs[root].quota != k->budget || k->budget < boot_budget) {
        (void)snprintf(why, whylen,
                       "I1/I3: teardown left %u objects, %u accounts, %u registrations, %u "
                       "spaces; root quota %u, budget %llu (all of it must be back in the root; "
                       "fixtures' donations to tasks reaped into it can add to the %llu booted)",
                       k->objects_live, accts, dns, spaces, k->cs[root].quota,
                       (unsigned long long)k->budget, (unsigned long long)boot_budget);
        return 0;
    }
    return 1;
}
