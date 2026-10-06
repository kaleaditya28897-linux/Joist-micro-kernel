/* test_props_ipc.c - property test for synchronous IPC (Phase 1 work item 2;
 * invariants I6, I7, I10).
 *
 * Random Call/Recv/Reply/ReplyRecv (no bare Send: 0001's amendment), deferred replies through reply
 * objects (work item 6), clock ticks, thread creation and destruction, and
 * capability deletion run against the model and an independent
 * shadow of 0001's IPC rules (queues as ordered lists, thread states,
 * messages, badges, deadlines, reply capabilities). After every step the two
 * must agree on the return code, every thread field and every queue's order,
 * and joist_check_invariants must be clean.
 *
 * Capability lookups in the shadow read the model's capability table: that
 * part is property-tested by test_props_cspace.c, and IPC is the subject here.
 *
 * Same environment variables as test_props_cspace.c (JOIST_PROP_SEEDS,
 * JOIST_PROP_STEPS, JOIST_PROP_SEED). */
#include "harness.h"
#include "joist/core.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng_state;

static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static uint32_t pick(uint32_t n)
{
    return (uint32_t)(rng() % n);
}

/* ---- shadow ---- */

struct sh_th {
    uint8_t state;
    uint32_t cs;
    uint32_t ep; /* object index + 1 while queued */
    uint64_t deadline;
    uint32_t call_seq;
    struct joist_msg msg;
    joist_badge badge;
    joist_err result;
    uint32_t reply_to;
    uint32_t reply_seq;
};

static struct sh_th sh[JOIST_MAX_THREADS];
static uint32_t shq[JOIST_MAX_OBJECTS][JOIST_MAX_THREADS]; /* queue: thread indexes in order */
static uint32_t shq_len[JOIST_MAX_OBJECTS];
static uint8_t shq_kind[JOIST_MAX_OBJECTS];
static uint64_t sh_now;
static uint8_t sh_dead[JOIST_MAX_OBJECTS]; /* endpoint death already processed */
/* Reply objects, by model object index: the saved reply capability. */
static struct {
    uint32_t target; /* caller + 1, 0 = unbound */
    uint32_t seq;
} sh_rep[JOIST_MAX_OBJECTS];
static struct joist_kernel k;

/* An endpoint is dead exactly when no capability with RECV names it: the
 * last receiver deleted it, and derive can never add RECV back (I8). */
static int sh_recv_caps(uint32_t o)
{
    int n = 0;
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
            const struct joist_cap *cap = &k.cs[c].slot[i];
            if (cap->obj == o + 1u && (cap->rights & JOIST_R_RECV) != 0) {
                n++;
            }
        }
    }
    return n;
}

static uint64_t sh_deadline(uint64_t timeout)
{
    return (timeout == JOIST_FOREVER || sh_now > JOIST_FOREVER - timeout) ? JOIST_FOREVER
                                                                          : sh_now + timeout;
}

#define N_WAKE 16u
static unsigned long wake_cov[N_WAKE]; /* wake-ups by outcome (I10 paths) */

static void sh_wake(uint32_t t, joist_err r)
{
    wake_cov[(unsigned)r < N_WAKE ? (unsigned)r : N_WAKE - 1u]++;
    sh[t].state = JOIST_T_RUNNING;
    sh[t].deadline = JOIST_FOREVER;
    sh[t].result = r;
}

static void shq_remove(uint32_t t)
{
    uint32_t o = sh[t].ep - 1u;
    uint32_t j = 0;
    for (uint32_t i = 0; i < shq_len[o]; i++) {
        if (shq[o][i] != t) {
            shq[o][j++] = shq[o][i];
        }
    }
    shq_len[o] = j;
    if (j == 0) {
        shq_kind[o] = JOIST_Q_NONE;
    }
    sh[t].ep = 0;
}

static void shq_push(uint32_t o, uint8_t kind, uint32_t t)
{
    if (shq_len[o] == 0) {
        shq_kind[o] = kind;
    }
    shq[o][shq_len[o]++] = t;
    sh[t].ep = o + 1u;
}

static uint32_t shq_pop(uint32_t o)
{
    uint32_t t = shq[o][0];
    shq_remove(t);
    return t;
}

static joist_err sh_running(uint32_t t)
{
    if (t >= JOIST_MAX_THREADS) {
        return JOIST_E_RANGE;
    }
    return sh[t].state == JOIST_T_RUNNING ? JOIST_OK : JOIST_E_STATE;
}

static joist_err sh_lookup(uint32_t t, uint32_t slot, joist_rights need, uint32_t *obj,
                           joist_badge *badge)
{
    if (slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *c = &k.cs[sh[t].cs].slot[slot];
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (c->type != JOIST_OBJ_ENDPOINT) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & need) == 0) {
        return JOIST_E_RIGHTS;
    }
    if (sh_recv_caps(c->obj - 1u) == 0) {
        return JOIST_E_GONE; /* I5 */
    }
    *obj = c->obj - 1u;
    *badge = c->badge;
    return JOIST_OK;
}

static void sh_mint_reply(uint32_t server, uint32_t c)
{
    sh[c].call_seq++;
    sh[c].state = JOIST_T_BLOCKED_REPLY;
    sh[server].reply_to = c + 1u;
    sh[server].reply_seq = sh[c].call_seq;
}

static joist_err sh_call(uint32_t t, uint32_t slot, const struct joist_msg *m, uint64_t timeout)
{
    uint32_t o;
    joist_badge b;
    joist_err e = sh_running(t);
    if (e != JOIST_OK) {
        return e;
    }
    if (timeout == 0) {
        return JOIST_E_TIMEOUT;
    }
    e = sh_lookup(t, slot, JOIST_R_SEND, &o, &b);
    if (e != JOIST_OK) {
        return e;
    }
    sh[t].deadline = sh_deadline(timeout);
    if (shq_kind[o] == JOIST_Q_RECEIVERS) {
        uint32_t r = shq_pop(o);
        sh[r].msg = *m;
        sh[r].badge = b;
        sh_wake(r, JOIST_OK);
        sh_mint_reply(r, t);
        return JOIST_BLOCKED;
    }
    sh[t].msg = *m;
    sh[t].badge = b;
    sh[t].state = JOIST_T_BLOCKED_SEND;
    shq_push(o, JOIST_Q_SENDERS, t);
    return JOIST_BLOCKED;
}

static joist_err sh_recv(uint32_t t, uint32_t slot, uint64_t timeout)
{
    uint32_t o;
    joist_badge b;
    joist_err e = sh_running(t);
    if (e != JOIST_OK) {
        return e;
    }
    if (sh[t].reply_to != 0) {
        return JOIST_E_STATE;
    }
    e = sh_lookup(t, slot, JOIST_R_RECV, &o, &b);
    if (e != JOIST_OK) {
        return e;
    }
    if (shq_kind[o] == JOIST_Q_SENDERS) {
        uint32_t s = shq_pop(o);
        sh[t].msg = sh[s].msg;
        sh[t].badge = sh[s].badge;
        sh[t].result = JOIST_OK;
        sh_mint_reply(t, s);
        return JOIST_OK;
    }
    if (timeout == 0) {
        return JOIST_E_TIMEOUT;
    }
    sh[t].state = JOIST_T_BLOCKED_RECV;
    sh[t].deadline = sh_deadline(timeout);
    shq_push(o, JOIST_Q_RECEIVERS, t);
    return JOIST_BLOCKED;
}

static joist_err sh_reply(uint32_t t, const struct joist_msg *m)
{
    joist_err e = sh_running(t);
    if (e != JOIST_OK) {
        return e;
    }
    if (sh[t].reply_to == 0) {
        return JOIST_E_NOREPLY;
    }
    uint32_t c = sh[t].reply_to - 1u;
    uint32_t seq = sh[t].reply_seq;
    sh[t].reply_to = 0;
    sh[t].reply_seq = 0;
    if (sh[c].state != JOIST_T_BLOCKED_REPLY || sh[c].call_seq != seq) {
        return JOIST_E_PEER;
    }
    sh[c].msg = *m;
    sh[c].badge = 0;
    sh_wake(c, JOIST_OK);
    return JOIST_OK;
}

static joist_err sh_reply_obj(uint32_t t, uint32_t slot, uint32_t *o)
{
    if (slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *c = &k.cs[sh[t].cs].slot[slot];
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (c->type != JOIST_OBJ_REPLY) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & JOIST_R_SEND) == 0) {
        return JOIST_E_RIGHTS;
    }
    *o = c->obj - 1u;
    return JOIST_OK;
}

static joist_err sh_save(uint32_t t, uint32_t slot)
{
    uint32_t o;
    joist_err e = sh_running(t);
    if (e == JOIST_OK) {
        e = sh_reply_obj(t, slot, &o);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (sh[t].reply_to == 0) {
        return JOIST_E_NOREPLY;
    }
    if (sh_rep[o].target != 0) {
        return JOIST_E_OCCUPIED;
    }
    sh_rep[o].target = sh[t].reply_to;
    sh_rep[o].seq = sh[t].reply_seq;
    sh[t].reply_to = 0;
    sh[t].reply_seq = 0;
    return JOIST_OK;
}

static joist_err sh_saved(uint32_t t, uint32_t slot, const struct joist_msg *m)
{
    uint32_t o;
    joist_err e = sh_running(t);
    if (e == JOIST_OK) {
        e = sh_reply_obj(t, slot, &o);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (sh_rep[o].target == 0) {
        return JOIST_E_NOREPLY;
    }
    uint32_t c = sh_rep[o].target - 1u;
    uint32_t seq = sh_rep[o].seq;
    sh_rep[o].target = 0;
    sh_rep[o].seq = 0;
    if (sh[c].state != JOIST_T_BLOCKED_REPLY || sh[c].call_seq != seq) {
        return JOIST_E_PEER;
    }
    sh[c].msg = *m;
    sh[c].badge = 0;
    sh_wake(c, JOIST_OK);
    return JOIST_OK;
}

/* After a capability deletion: a reply object that was freed while bound
 * wakes its caller (I10). */
static void sh_reap_replies(void)
{
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        if (k.obj[o].type == JOIST_OBJ_NONE && sh_rep[o].target != 0) {
            uint32_t c = sh_rep[o].target - 1u;
            if (sh[c].state == JOIST_T_BLOCKED_REPLY && sh[c].call_seq == sh_rep[o].seq) {
                sh_wake(c, JOIST_E_PEER);
            }
            sh_rep[o].target = 0;
            sh_rep[o].seq = 0;
        }
    }
}

static joist_err sh_reply_recv(uint32_t t, const struct joist_msg *m, uint32_t slot,
                               uint64_t timeout)
{
    joist_err e = sh_running(t);
    if (e != JOIST_OK) {
        return e;
    }
    if (sh[t].reply_to != 0) {
        (void)sh_reply(t, m);
    }
    return sh_recv(t, slot, timeout);
}

static void sh_tick(uint64_t dt)
{
    sh_now = (sh_now > JOIST_FOREVER - dt) ? JOIST_FOREVER : sh_now + dt;
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        if (sh[i].state == JOIST_T_FREE || sh[i].state == JOIST_T_RUNNING ||
            sh[i].deadline == JOIST_FOREVER || sh[i].deadline > sh_now) {
            continue;
        }
        if (sh[i].state == JOIST_T_BLOCKED_SEND || sh[i].state == JOIST_T_BLOCKED_RECV) {
            shq_remove(i);
        }
        sh_wake(i, JOIST_E_TIMEOUT);
    }
}

static joist_err sh_thread_destroy(uint32_t t)
{
    if (t >= JOIST_MAX_THREADS) {
        return JOIST_E_RANGE;
    }
    if (sh[t].state == JOIST_T_FREE) {
        return JOIST_E_STATE;
    }
    if (sh[t].state == JOIST_T_BLOCKED_SEND || sh[t].state == JOIST_T_BLOCKED_RECV) {
        shq_remove(t);
    }
    if (sh[t].reply_to != 0) {
        uint32_t c = sh[t].reply_to - 1u;
        if (sh[c].state == JOIST_T_BLOCKED_REPLY && sh[c].call_seq == sh[t].reply_seq) {
            sh_wake(c, JOIST_E_PEER);
        }
    }
    uint32_t seq = sh[t].call_seq + 1u;
    memset(&sh[t], 0, sizeof sh[t]);
    sh[t].call_seq = seq;
    return JOIST_OK;
}

static joist_err sh_thread_create(uint32_t cs, uint32_t *out)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    if (!k.cs[cs].live) {
        return JOIST_E_DEAD;
    }
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        if (sh[i].state == JOIST_T_FREE) {
            uint32_t seq = sh[i].call_seq;
            memset(&sh[i], 0, sizeof sh[i]);
            sh[i].call_seq = seq;
            sh[i].state = JOIST_T_RUNNING;
            sh[i].cs = cs;
            sh[i].deadline = JOIST_FOREVER;
            *out = i;
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

/* After a capability operation: an endpoint whose last RECV capability just
 * went dies, and its queue wakes with E_GONE (I5). A freed object slot may be
 * reused, so its death mark is cleared. */
static void sh_reap_endpoints(void)
{
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        if (k.obj[o].type == JOIST_OBJ_NONE) {
            sh_dead[o] = 0;
        }
        int dies = !sh_dead[o] && sh_recv_caps(o) == 0 &&
                   (k.obj[o].type == JOIST_OBJ_ENDPOINT || shq_len[o] != 0);
        if (dies || (k.obj[o].type == JOIST_OBJ_NONE && shq_len[o] != 0)) {
            while (shq_len[o] != 0) {
                uint32_t t = shq_pop(o);
                sh_wake(t, JOIST_E_GONE);
            }
            if (k.obj[o].type == JOIST_OBJ_ENDPOINT) {
                sh_dead[o] = 1;
            }
        }
    }
}

/* ---- comparison ---- */

static char why[256];

static int compare(void)
{
    if (k.now != sh_now) {
        (void)snprintf(why, sizeof why, "now %" PRIu64 ", shadow %" PRIu64, k.now, sh_now);
        return 0;
    }
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        const struct joist_thread_rec *r = &k.th[t];
        const struct sh_th *s = &sh[t];
        if (r->state != s->state || r->call_seq != s->call_seq) {
            (void)snprintf(why, sizeof why, "thread %u: state %u/%u call_seq %u/%u", t, r->state,
                           s->state, r->call_seq, s->call_seq);
            return 0;
        }
        if (r->state == JOIST_T_FREE) {
            continue;
        }
        if (r->cs != s->cs || r->ep != s->ep || r->deadline != s->deadline ||
            r->result != s->result || r->reply_to != s->reply_to || r->reply_seq != s->reply_seq ||
            r->badge != s->badge || memcmp(&r->msg, &s->msg, sizeof r->msg) != 0) {
            (void)snprintf(why, sizeof why,
                           "thread %u: cs %u/%u ep %u/%u result %s/%s reply %u.%u/%u.%u "
                           "badge %" PRIu64 "/%" PRIu64 " msg %" PRIu64 "/%" PRIu64,
                           t, r->cs, s->cs, r->ep, s->ep, joist_err_name(r->result),
                           joist_err_name(s->result), r->reply_to, r->reply_seq, s->reply_to,
                           s->reply_seq, r->badge, s->badge, r->msg.r[0], s->msg.r[0]);
            return 0;
        }
    }
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        if (k.obj[o].type == JOIST_OBJ_REPLY &&
            (k.obj[o].target != sh_rep[o].target || k.obj[o].rseq != sh_rep[o].seq)) {
            (void)snprintf(why, sizeof why, "reply object %u: bound to %u.%u, shadow %u.%u", o,
                           k.obj[o].target, k.obj[o].rseq, sh_rep[o].target, sh_rep[o].seq);
            return 0;
        }
        if (k.obj[o].qkind != shq_kind[o]) {
            (void)snprintf(why, sizeof why, "endpoint %u: queue kind %u/%u", o, k.obj[o].qkind,
                           shq_kind[o]);
            return 0;
        }
        uint32_t cur = k.obj[o].qhead;
        for (uint32_t i = 0; i < shq_len[o]; i++) {
            if (cur == 0 || cur - 1u != shq[o][i]) {
                (void)snprintf(why, sizeof why, "endpoint %u: queue position %u differs", o, i);
                return 0;
            }
            cur = k.th[cur - 1u].qnext;
        }
        if (cur != 0) {
            (void)snprintf(why, sizeof why, "endpoint %u: model queue longer than shadow", o);
            return 0;
        }
    }
    return joist_check_invariants(&k, why, sizeof why) == 0;
}

/* ---- world and run ---- */

/* Each space gets endpoints with full, send-only (badged) and receive-only
 * capabilities in fixed slots; deleted capabilities are re-created by the
 * generator so the world doesn't die out. */
enum { SLOTS_PER_EP = 3, N_EP = 3, N_SPACES = 2, RPL_SLOT = N_EP * SLOTS_PER_EP, N_RPL = 2 };

static void ensure_replies(uint32_t cs)
{
    for (uint32_t i = 0; i < N_RPL; i++) {
        if (k.cs[cs].slot[RPL_SLOT + i].obj == 0) {
            (void)joist_obj_create(&k, cs, JOIST_OBJ_REPLY, RPL_SLOT + i);
        }
    }
}

static void ensure_endpoint(uint32_t cs, uint32_t e)
{
    joist_slot base = e * SLOTS_PER_EP;
    if (k.cs[cs].slot[base].obj == 0 && k.cs[cs].slot[base + 1u].obj == 0 &&
        k.cs[cs].slot[base + 2u].obj == 0) {
        (void)joist_obj_create(&k, cs, JOIST_OBJ_ENDPOINT, base);
        (void)joist_cap_mint(&k, cs, base, base + 1u, JOIST_R_SEND,
                             (joist_badge)(100u * cs + 10u * e + 1u));
        (void)joist_cap_derive(&k, cs, base, base + 2u, JOIST_R_RECV);
    }
}

enum {
    OP_CALL,
    OP_RECV,
    OP_REPLY,
    OP_REPLY_RECV,
    OP_TICK,
    OP_TH_CREATE,
    OP_TH_DESTROY,
    OP_CAP_DELETE,
    OP_SAVE,
    OP_SAVED,
    OP_COUNT
};
static const char *const op_name[OP_COUNT] = {
    "call",      "recv",       "reply",      "reply_recv", "tick",
    "th_create", "th_destroy", "cap_delete", "reply_save", "reply_saved"};
#define N_RES 16u
static unsigned long coverage[OP_COUNT][N_RES];

static unsigned long env_ulong(const char *name, unsigned long def)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? strtoul(v, NULL, 0) : def;
}

static uint64_t pick_timeout(void)
{
    uint32_t r = pick(10);
    return r < 2 ? 0 : r < 4 ? JOIST_FOREVER : (uint64_t)(1u + pick(20));
}

static int run_seed(uint64_t seed, unsigned long steps)
{
    rng_state = seed * 0x9E3779B97F4A7C15ULL + 7u;
    joist_init(&k);
    memset(sh, 0, sizeof sh);
    memset(shq_len, 0, sizeof shq_len);
    memset(shq_kind, 0, sizeof shq_kind);
    memset(sh_dead, 0, sizeof sh_dead);
    memset(sh_rep, 0, sizeof sh_rep);
    sh_now = 0;

    for (uint32_t c = 0; c < N_SPACES; c++) {
        joist_cspace cs;
        if (joist_cspace_create(&k, 64, &cs) != JOIST_OK) {
            return 0;
        }
    }
    for (unsigned long step = 0; step < steps; step++) {
        for (uint32_t c = 0; c < N_SPACES; c++) {
            for (uint32_t e = 0; e < N_EP; e++) {
                if (pick(8) == 0) {
                    ensure_endpoint(c, e);
                }
            }
            if (pick(8) == 0) {
                ensure_replies(c);
            }
        }
        /* A thread index, usually a live one, occasionally out of range. */
        uint32_t t = pick(JOIST_MAX_THREADS + 1u);
        if (pick(4) != 0) {
            uint32_t start = pick(JOIST_MAX_THREADS);
            for (uint32_t n = 0; n < JOIST_MAX_THREADS; n++) {
                uint32_t i = (start + n) % JOIST_MAX_THREADS;
                if (sh[i].state == JOIST_T_RUNNING) {
                    t = i;
                    break;
                }
            }
        }
        uint32_t slot = pick(N_EP * SLOTS_PER_EP + 2u); /* includes empty slots */
        struct joist_msg m = {{0}};
        m.r[0] = rng();
        m.r[JOIST_MSG_REGS - 1u] = step;
        uint64_t timeout = pick_timeout();
        uint32_t roll = pick(110);
        int op;
        joist_err got;
        joist_err want;

        if (roll < 32) {
            op = OP_CALL;
            want = sh_call(t, slot, &m, timeout);
            got = joist_call(&k, t, slot, &m, timeout);
        } else if (roll < 50) {
            op = OP_RECV;
            want = sh_recv(t, slot, timeout);
            got = joist_recv(&k, t, slot, timeout);
        } else if (roll < 60) {
            op = OP_REPLY;
            want = sh_reply(t, &m);
            got = joist_reply(&k, t, &m);
        } else if (roll < 70) {
            op = OP_REPLY_RECV;
            want = sh_reply_recv(t, &m, slot, timeout);
            got = joist_reply_recv(&k, t, &m, slot, timeout);
        } else if (roll < 82) {
            op = OP_TICK;
            uint64_t dt = pick(20) == 0 ? (uint64_t)pick(1000) : (uint64_t)pick(6);
            sh_tick(dt);
            joist_tick(&k, dt);
            got = want = JOIST_OK;
        } else if (roll < 90) {
            op = OP_TH_CREATE;
            uint32_t cs = pick(N_SPACES + 1u);
            uint32_t a = 0, b = 0;
            want = sh_thread_create(cs, &a);
            got = joist_thread_create(&k, cs, &b);
            if (got == JOIST_OK && want == JOIST_OK && a != b) {
                (void)snprintf(why, sizeof why, "thread_create picked %u, shadow %u", b, a);
                got = JOIST_E_RANGE;
            }
        } else if (roll < 95) {
            op = OP_TH_DESTROY;
            uint32_t victim = pick(JOIST_MAX_THREADS + 1u);
            want = sh_thread_destroy(victim);
            got = joist_thread_destroy(&k, victim);
        } else if (roll < 97) {
            op = OP_CAP_DELETE;
            uint32_t cs = pick(N_SPACES);
            want = k.cs[cs].slot[slot < JOIST_CSPACE_SLOTS ? slot : 0].obj != 0 ? JOIST_OK
                                                                                : JOIST_E_EMPTY;
            if (slot >= JOIST_CSPACE_SLOTS) {
                want = JOIST_E_RANGE;
            }
            got = joist_cap_delete(&k, cs, slot);
            sh_reap_endpoints();
            sh_reap_replies();
        } else if (roll < 100) {
            /* Deleting a reply object: the bound-and-freed path (I10). */
            op = OP_CAP_DELETE;
            uint32_t cs = pick(N_SPACES);
            uint32_t rs = RPL_SLOT + pick(N_RPL);
            want = k.cs[cs].slot[rs].obj != 0 ? JOIST_OK : JOIST_E_EMPTY;
            got = joist_cap_delete(&k, cs, rs);
            sh_reap_endpoints();
            sh_reap_replies();
        } else {
            /* A thread holding a reply capability, usually, so saves succeed. */
            for (uint32_t n = 0; n < JOIST_MAX_THREADS && pick(4) != 0; n++) {
                uint32_t i = (t + n) % JOIST_MAX_THREADS;
                if (sh[i].state == JOIST_T_RUNNING && sh[i].reply_to != 0) {
                    t = i;
                    break;
                }
            }
            uint32_t rs = pick(4) == 0 ? slot : RPL_SLOT + pick(N_RPL);
            if (roll < 105) {
                op = OP_SAVE;
                want = sh_save(t, rs);
                got = joist_reply_save(&k, t, rs);
            } else {
                op = OP_SAVED;
                want = sh_saved(t, rs, &m);
                got = joist_reply_saved(&k, t, rs, &m);
            }
        }

        coverage[op][(unsigned)got < N_RES ? (unsigned)got : N_RES - 1u]++;
        if (got != want) {
            fprintf(stderr, "  seed %" PRIu64 " step %lu: %s returned %s, shadow %s %s\n", seed,
                    step, op_name[op], joist_err_name(got), joist_err_name(want), why);
            return 0;
        }
        if (!compare()) {
            fprintf(stderr, "  seed %" PRIu64 " step %lu after %s: %s\n", seed, step, op_name[op],
                    why);
            return 0;
        }
    }
    /* End of run: advance past every finite deadline; nothing with a deadline
     * may still be blocked (I10). */
    sh_tick(JOIST_FOREVER);
    joist_tick(&k, JOIST_FOREVER);
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        if (k.th[t].state != JOIST_T_FREE && k.th[t].state != JOIST_T_RUNNING &&
            k.th[t].deadline != JOIST_FOREVER) {
            fprintf(stderr, "  seed %" PRIu64 ": I10: thread %u still blocked past its deadline\n",
                    seed, t);
            return 0;
        }
    }
    if (!compare()) {
        fprintf(stderr, "  seed %" PRIu64 " at the end: %s\n", seed, why);
        return 0;
    }
    /* Destroying every space must leave no thread and no object (I1). */
    for (uint32_t c = 0; c < N_SPACES; c++) {
        (void)joist_cspace_destroy(&k, c);
    }
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        if (k.th[t].state != JOIST_T_FREE) {
            fprintf(stderr, "  seed %" PRIu64 ": thread %u survived its space\n", seed, t);
            return 0;
        }
    }
    if (k.objects_live != 0 || joist_check_invariants(&k, why, sizeof why) != 0) {
        fprintf(stderr, "  seed %" PRIu64 ": after teardown: live %u %s\n", seed, k.objects_live,
                why);
        return 0;
    }
    return 1;
}

JT_TEST(props_ipc_model_matches_shadow)
{
    unsigned long steps = env_ulong("JOIST_PROP_STEPS", 2000);
    const char *one = getenv("JOIST_PROP_SEED");
    memset(coverage, 0, sizeof coverage);
    memset(wake_cov, 0, sizeof wake_cov);
    if (one != NULL && *one != '\0') {
        JT_CHECK(run_seed(strtoull(one, NULL, 0), steps));
    } else {
        unsigned long seeds = env_ulong("JOIST_PROP_SEEDS", 200);
        for (unsigned long s = 1; s <= seeds; s++) {
            if (!run_seed(s, steps)) {
                JT_CHECK(0 && "property failed; replay with JOIST_PROP_SEED");
                break;
            }
        }
    }
    for (int op = 0; op < OP_COUNT; op++) {
        fprintf(stderr, "  coverage %-11s", op_name[op]);
        for (unsigned e = 0; e < N_RES; e++) {
            if (coverage[op][e] != 0) {
                fprintf(stderr, " %s=%lu", joist_err_name((joist_err)e), coverage[op][e]);
            }
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  coverage wake-ups   ");
    for (unsigned e = 0; e < N_WAKE; e++) {
        if (wake_cov[e] != 0) {
            fprintf(stderr, " %s=%lu", joist_err_name((joist_err)e), wake_cov[e]);
        }
    }
    fprintf(stderr, "\n");
}
