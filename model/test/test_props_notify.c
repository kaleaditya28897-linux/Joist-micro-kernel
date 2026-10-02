/* test_props_notify.c - property test for notifications and lifetime events
 * (Phase 1 work item 3; invariants I2, I4, I5).
 *
 * Random object creation, mint/derive/delete, no-senders binding, dead-name
 * requests, Signal, Wait, clock ticks and thread churn run against the model
 * and a shadow that predicts, independently: every notification's word and
 * waiter queue, every endpoint's death, every no-senders firing (from
 * per-badge send counts it computes itself), every dead-name registration and
 * its single firing, and the cleanup when a notification is freed. The
 * effects of one delete happen in the model's order: no-senders, then
 * endpoint death, then the object being freed.
 *
 * The capability operations' own return codes come from the model (their
 * rules are property-tested in test_props_cspace.c); what is predicted here is
 * their consequence for lifetime events. Same environment variables as the
 * other property tests. */
#include "harness.h"
#include "joist/model.h"

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

static struct joist_kernel k;
static joist_cspace cs;
enum { N_THREADS = 4, N_SLOTS = 24 };

/* ---- shadow ---- */

struct sh_th {
    uint8_t state; /* RUNNING, BLOCKED_WAIT or FREE */
    uint64_t deadline;
    joist_badge badge;
    joist_err result;
};
static struct sh_th sh[JOIST_MAX_THREADS];
static uint64_t sh_word[JOIST_MAX_OBJECTS];
static uint32_t sh_q[JOIST_MAX_OBJECTS][JOIST_MAX_THREADS];
static uint32_t sh_qlen[JOIST_MAX_OBJECTS];
static uint8_t sh_dead[JOIST_MAX_OBJECTS];
static uint32_t sh_bind[JOIST_MAX_OBJECTS]; /* notification index + 1 */
static struct joist_deadname sh_dn[JOIST_MAX_DEADNAMES];
static uint64_t sh_now;

#define N_WAKE 16u
static unsigned long wake_cov[N_WAKE];
static unsigned long fired_nosenders, fired_deadname, endpoint_deaths, notes_freed;

static void sh_wake(uint32_t t, joist_err r)
{
    wake_cov[(unsigned)r < N_WAKE ? (unsigned)r : N_WAKE - 1u]++;
    sh[t].state = JOIST_T_RUNNING;
    sh[t].deadline = JOIST_FOREVER;
    sh[t].result = r;
}

static void sh_q_remove(uint32_t o, uint32_t t)
{
    uint32_t j = 0;
    for (uint32_t i = 0; i < sh_qlen[o]; i++) {
        if (sh_q[o][i] != t) {
            sh_q[o][j++] = sh_q[o][i];
        }
    }
    sh_qlen[o] = j;
}

static uint32_t sh_waiting_on(uint32_t t) /* object + 1, or 0 */
{
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        for (uint32_t i = 0; i < sh_qlen[o]; i++) {
            if (sh_q[o][i] == t) {
                return o + 1u;
            }
        }
    }
    return 0;
}

static void sh_wake_all(uint32_t o, joist_err r)
{
    while (sh_qlen[o] != 0) {
        uint32_t t = sh_q[o][0];
        sh_q_remove(o, t);
        sh_wake(t, r);
    }
}

static void sh_signal(uint32_t o, uint64_t bits)
{
    if (sh_qlen[o] != 0) {
        uint32_t w = sh_q[o][0];
        sh_q_remove(o, w);
        sh[w].badge = bits;
        sh_wake(w, JOIST_OK);
        return;
    }
    sh_word[o] |= bits;
}

/* Counts computed from the model's capability table. */
static uint32_t count_caps(uint32_t o)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
        n += (uint32_t)(k.cs[cs].slot[i].obj == o + 1u);
    }
    return n;
}

static uint32_t count_recv(uint32_t o)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
        const struct joist_cap *c = &k.cs[cs].slot[i];
        n += (uint32_t)(c->obj == o + 1u && (c->rights & JOIST_R_RECV) != 0);
    }
    return n;
}

static uint32_t count_send(uint32_t o, joist_badge b)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
        const struct joist_cap *c = &k.cs[cs].slot[i];
        n += (uint32_t)(c->obj == o + 1u && c->badge == b && b != 0 &&
                        (c->rights & JOIST_R_SEND) != 0);
    }
    return n;
}

/* The consequences of deleting capability `gone` (already removed from the
 * model's table when this runs), in the model's order. */
static void sh_after_delete(const struct joist_cap *gone)
{
    uint32_t o = gone->obj - 1u;
    if (gone->type == JOIST_OBJ_ENDPOINT) {
        if ((gone->rights & JOIST_R_SEND) != 0 && gone->badge != 0 &&
            count_send(o, gone->badge) == 0 && !sh_dead[o] && sh_bind[o] != 0) {
            sh_signal(sh_bind[o] - 1u, gone->badge); /* I4 */
            fired_nosenders++;
        }
        if ((gone->rights & JOIST_R_RECV) != 0 && count_recv(o) == 0 && !sh_dead[o]) {
            sh_dead[o] = 1; /* I5 */
            endpoint_deaths++;
            sh_wake_all(o, JOIST_E_GONE);
            for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
                if (sh_dn[i].ep == o + 1u) {
                    uint32_t n = sh_dn[i].notif - 1u;
                    uint64_t bits = sh_dn[i].bits;
                    memset(&sh_dn[i], 0, sizeof sh_dn[i]);
                    sh_signal(n, bits);
                    fired_deadname++;
                }
            }
        }
    }
    if (count_caps(o) == 0) { /* the object is freed */
        sh_wake_all(o, JOIST_E_GONE);
        if (gone->type == JOIST_OBJ_NOTIFICATION) {
            notes_freed++;
            for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
                if (sh_bind[i] == o + 1u) {
                    sh_bind[i] = 0;
                }
            }
            for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
                if (sh_dn[i].notif == o + 1u) {
                    memset(&sh_dn[i], 0, sizeof sh_dn[i]);
                }
            }
        } else {
            for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
                if (sh_dn[i].ep == o + 1u) {
                    memset(&sh_dn[i], 0, sizeof sh_dn[i]);
                }
            }
        }
        sh_word[o] = 0;
        sh_dead[o] = 0;
        sh_bind[o] = 0;
    }
}

static joist_err sh_note_lookup(uint32_t slot, joist_rights need, uint32_t *o, joist_badge *b)
{
    if (slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *c = &k.cs[cs].slot[slot];
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (c->type != JOIST_OBJ_NOTIFICATION) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & need) == 0) {
        return JOIST_E_RIGHTS;
    }
    *o = c->obj - 1u;
    *b = c->badge;
    return JOIST_OK;
}

static joist_err sh_running(uint32_t t)
{
    if (t >= JOIST_MAX_THREADS) {
        return JOIST_E_RANGE;
    }
    return sh[t].state == JOIST_T_RUNNING ? JOIST_OK : JOIST_E_STATE;
}

static joist_err sh_do_signal(uint32_t t, uint32_t slot)
{
    uint32_t o;
    joist_badge b;
    joist_err e = sh_running(t);
    if (e == JOIST_OK) {
        e = sh_note_lookup(slot, JOIST_R_SEND, &o, &b);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (b == 0) {
        return JOIST_E_BADGE;
    }
    sh_signal(o, b);
    return JOIST_OK;
}

static joist_err sh_do_wait(uint32_t t, uint32_t slot, uint64_t timeout)
{
    uint32_t o;
    joist_badge b;
    joist_err e = sh_running(t);
    if (e == JOIST_OK) {
        e = sh_note_lookup(slot, JOIST_R_RECV, &o, &b);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (sh_word[o] != 0) {
        sh[t].badge = sh_word[o];
        sh_word[o] = 0;
        sh[t].result = JOIST_OK;
        return JOIST_OK;
    }
    if (timeout == 0) {
        return JOIST_E_TIMEOUT;
    }
    sh[t].state = JOIST_T_BLOCKED_WAIT;
    sh[t].deadline = (timeout == JOIST_FOREVER || sh_now > JOIST_FOREVER - timeout)
                         ? JOIST_FOREVER
                         : sh_now + timeout;
    sh_q[o][sh_qlen[o]++] = t;
    return JOIST_BLOCKED;
}

static joist_err sh_ep_cap(uint32_t slot, const struct joist_cap **out)
{
    if (slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *c = &k.cs[cs].slot[slot];
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (c->type != JOIST_OBJ_ENDPOINT) {
        return JOIST_E_TYPE;
    }
    *out = c;
    return JOIST_OK;
}

static joist_err sh_do_bind(uint32_t ep_slot, uint32_t note_slot)
{
    const struct joist_cap *ec;
    uint32_t o;
    joist_badge b;
    joist_err e = sh_ep_cap(ep_slot, &ec);
    if (e != JOIST_OK) {
        return e;
    }
    if ((ec->rights & JOIST_R_RECV) == 0) {
        return JOIST_E_RIGHTS;
    }
    e = sh_note_lookup(note_slot, JOIST_R_SEND, &o, &b);
    if (e != JOIST_OK) {
        return e;
    }
    sh_bind[ec->obj - 1u] = o + 1u;
    return JOIST_OK;
}

static joist_err sh_do_deadname(uint32_t ep_slot, uint32_t note_slot)
{
    const struct joist_cap *ec;
    uint32_t o;
    joist_badge b;
    joist_err e = sh_ep_cap(ep_slot, &ec);
    if (e != JOIST_OK) {
        return e;
    }
    e = sh_note_lookup(note_slot, JOIST_R_SEND, &o, &b);
    if (e != JOIST_OK) {
        return e;
    }
    if (b == 0) {
        return JOIST_E_BADGE;
    }
    if (sh_dead[ec->obj - 1u]) {
        return JOIST_E_GONE;
    }
    for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
        if (sh_dn[i].ep == 0) {
            sh_dn[i] = (struct joist_deadname){.ep = ec->obj, .notif = o + 1u, .bits = b};
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

static void sh_tick(uint64_t dt)
{
    sh_now = (sh_now > JOIST_FOREVER - dt) ? JOIST_FOREVER : sh_now + dt;
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        if (sh[t].state == JOIST_T_BLOCKED_WAIT && sh[t].deadline != JOIST_FOREVER &&
            sh[t].deadline <= sh_now) {
            sh_q_remove(sh_waiting_on(t) - 1u, t);
            sh_wake(t, JOIST_E_TIMEOUT);
        }
    }
}

/* ---- comparison ---- */

static char why[256];

static int compare(void)
{
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        const struct joist_thread_rec *r = &k.th[t];
        if (r->state != sh[t].state) {
            (void)snprintf(why, sizeof why, "thread %u: state %u/%u", t, r->state, sh[t].state);
            return 0;
        }
        if (r->state != JOIST_T_FREE && (r->result != sh[t].result || r->badge != sh[t].badge ||
                                         r->deadline != sh[t].deadline)) {
            (void)snprintf(
                why, sizeof why, "thread %u: result %s/%s badge 0x%" PRIx64 "/0x%" PRIx64, t,
                joist_err_name(r->result), joist_err_name(sh[t].result), r->badge, sh[t].badge);
            return 0;
        }
    }
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *ob = &k.obj[o];
        if (ob->word != sh_word[o] || ob->dead != sh_dead[o] || ob->ns_notif != sh_bind[o]) {
            (void)snprintf(why, sizeof why,
                           "object %u: word 0x%" PRIx64 "/0x%" PRIx64 " dead %u/%u bind %u/%u", o,
                           ob->word, sh_word[o], ob->dead, sh_dead[o], ob->ns_notif, sh_bind[o]);
            return 0;
        }
        uint32_t cur = ob->qhead;
        for (uint32_t i = 0; i < sh_qlen[o]; i++) {
            if (cur == 0 || cur - 1u != sh_q[o][i]) {
                (void)snprintf(why, sizeof why, "object %u: queue position %u differs", o, i);
                return 0;
            }
            cur = k.th[cur - 1u].qnext;
        }
        if (cur != 0) {
            (void)snprintf(why, sizeof why, "object %u: model queue longer", o);
            return 0;
        }
    }
    for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
        if (memcmp(&k.dn[i], &sh_dn[i], sizeof sh_dn[i]) != 0) {
            (void)snprintf(why, sizeof why, "dead-name registration %u differs", i);
            return 0;
        }
    }
    return joist_check_invariants(&k, why, sizeof why) == 0;
}

/* ---- run ---- */

enum {
    OP_CREATE,
    OP_MINT,
    OP_DERIVE,
    OP_DELETE,
    OP_BIND,
    OP_DEADNAME,
    OP_SIGNAL,
    OP_WAIT,
    OP_TICK,
    OP_COUNT
};
static const char *const op_name[OP_COUNT] = {"create",   "mint",   "derive", "delete", "bind",
                                              "deadname", "signal", "wait",   "tick"};
#define N_RES 16u
static unsigned long coverage[OP_COUNT][N_RES];

static unsigned long env_ulong(const char *name, unsigned long def)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? strtoul(v, NULL, 0) : def;
}

static uint32_t occupied_slot(void)
{
    uint32_t start = pick(N_SLOTS);
    for (uint32_t n = 0; n < N_SLOTS; n++) {
        uint32_t i = (start + n) % N_SLOTS;
        if (k.cs[cs].slot[i].obj != 0) {
            return i;
        }
    }
    return pick(N_SLOTS);
}

static int run_seed(uint64_t seed, unsigned long steps)
{
    rng_state = seed * 0x9E3779B97F4A7C15ULL + 11u;
    joist_init(&k);
    memset(sh, 0, sizeof sh);
    memset(sh_word, 0, sizeof sh_word);
    memset(sh_qlen, 0, sizeof sh_qlen);
    memset(sh_dead, 0, sizeof sh_dead);
    memset(sh_bind, 0, sizeof sh_bind);
    memset(sh_dn, 0, sizeof sh_dn);
    sh_now = 0;
    if (joist_cspace_create(&k, 1000, &cs) != JOIST_OK) {
        return 0;
    }
    for (uint32_t i = 0; i < N_THREADS; i++) {
        joist_thread t;
        if (joist_thread_create(&k, cs, &t) != JOIST_OK) {
            return 0;
        }
        sh[t].state = JOIST_T_RUNNING;
        sh[t].deadline = JOIST_FOREVER;
    }

    for (unsigned long step = 0; step < steps; step++) {
        uint32_t roll = pick(100);
        uint32_t a = occupied_slot();
        uint32_t b = pick(4) == 0 ? pick(N_SLOTS) : occupied_slot();
        uint32_t t = pick(N_THREADS);
        int op;
        joist_err got = JOIST_OK, want = JOIST_OK;

        if (roll < 8) {
            op = OP_CREATE;
            joist_obj_type ty = pick(2) ? JOIST_OBJ_ENDPOINT : JOIST_OBJ_NOTIFICATION;
            got = want = joist_obj_create(&k, cs, ty, pick(N_SLOTS));
        } else if (roll < 22) {
            op = OP_MINT;
            joist_rights mask = (joist_rights)(pick(16) | (pick(2) ? JOIST_R_SEND : 0));
            got = want = joist_cap_mint(&k, cs, a, pick(N_SLOTS), mask,
                                        (joist_badge)1u << pick(8)); /* bit badges */
        } else if (roll < 32) {
            op = OP_DERIVE;
            got = want = joist_cap_derive(&k, cs, a, pick(N_SLOTS), (joist_rights)pick(16));
        } else if (roll < 47) {
            op = OP_DELETE;
            struct joist_cap gone = k.cs[cs].slot[a];
            got = want = joist_cap_delete(&k, cs, a);
            if (got == JOIST_OK) {
                sh_after_delete(&gone);
            }
        } else if (roll < 53) {
            op = OP_BIND;
            want = sh_do_bind(a, b);
            got = joist_bind_nosenders(&k, cs, a, b);
        } else if (roll < 60) {
            op = OP_DEADNAME;
            want = sh_do_deadname(a, b);
            got = joist_request_deadname(&k, cs, a, b);
        } else if (roll < 76) {
            op = OP_SIGNAL;
            want = sh_do_signal(t, a);
            got = joist_signal(&k, t, a);
        } else if (roll < 92) {
            op = OP_WAIT;
            uint64_t to = pick(3) == 0 ? 0 : pick(3) == 0 ? JOIST_FOREVER : 1u + pick(8);
            want = sh_do_wait(t, a, to);
            got = joist_wait(&k, t, a, to);
        } else {
            op = OP_TICK;
            uint64_t dt = pick(5);
            sh_tick(dt);
            joist_tick(&k, dt);
        }
        coverage[op][(unsigned)got < N_RES ? (unsigned)got : N_RES - 1u]++;
        if (got != want) {
            fprintf(stderr, "  seed %" PRIu64 " step %lu: %s returned %s, shadow %s\n", seed, step,
                    op_name[op], joist_err_name(got), joist_err_name(want));
            return 0;
        }
        if (!compare()) {
            fprintf(stderr, "  seed %" PRIu64 " step %lu after %s: %s\n", seed, step, op_name[op],
                    why);
            return 0;
        }
    }
    /* Tear down: destroying the space must leave nothing behind (I1). */
    (void)joist_cspace_destroy(&k, cs);
    if (k.objects_live != 0 || joist_check_invariants(&k, why, sizeof why) != 0) {
        fprintf(stderr, "  seed %" PRIu64 ": after teardown: live %u %s\n", seed, k.objects_live,
                why);
        return 0;
    }
    for (uint32_t i = 0; i < JOIST_MAX_BADGE_ACCTS; i++) {
        if (k.acct[i].obj != 0) {
            fprintf(stderr, "  seed %" PRIu64 ": account %u survived teardown\n", seed, i);
            return 0;
        }
    }
    for (uint32_t i = 0; i < JOIST_MAX_DEADNAMES; i++) {
        if (k.dn[i].ep != 0) {
            fprintf(stderr, "  seed %" PRIu64 ": dead-name %u survived teardown\n", seed, i);
            return 0;
        }
    }
    return 1;
}

JT_TEST(props_notify_model_matches_shadow)
{
    unsigned long steps = env_ulong("JOIST_PROP_STEPS", 2000);
    const char *one = getenv("JOIST_PROP_SEED");
    memset(coverage, 0, sizeof coverage);
    memset(wake_cov, 0, sizeof wake_cov);
    fired_nosenders = fired_deadname = endpoint_deaths = notes_freed = 0;
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
        fprintf(stderr, "  coverage %-9s", op_name[op]);
        for (unsigned e = 0; e < N_RES; e++) {
            if (coverage[op][e] != 0) {
                fprintf(stderr, " %s=%lu", joist_err_name((joist_err)e), coverage[op][e]);
            }
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "  coverage wake-ups  ");
    for (unsigned e = 0; e < N_WAKE; e++) {
        if (wake_cov[e] != 0) {
            fprintf(stderr, " %s=%lu", joist_err_name((joist_err)e), wake_cov[e]);
        }
    }
    fprintf(stderr,
            "\n  coverage events     no-senders fired=%lu dead-names fired=%lu "
            "endpoint deaths=%lu notifications freed=%lu\n",
            fired_nosenders, fired_deadname, endpoint_deaths, notes_freed);
}
