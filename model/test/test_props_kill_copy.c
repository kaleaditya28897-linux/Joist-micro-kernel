/* test_props_kill_copy.c - property test for revoke by kill and the batched
 * capability copy (decision 0022), with the operations around them: tasks
 * made by the root and destroyed (cascading), objects, derive, mint,
 * delete, leaves, frames split.
 *
 * Mapping is not among its operations since 0020: what a kill does to a
 * frame's mappings, and to the sweep that unmaps it, is covered by the
 * whole-model executor (exec.c: kills at random among real mappings, the
 * hooks' simulated tables and every invariant checked after each step),
 * by test_kill_copy.c and by user/revokeroot.
 *
 * A seeded generator issues random operations, valid and invalid, against
 * the model and against an independent shadow written as plainly as
 * possible. After every step the two must agree on the result (and the
 * batch's done count and skipped bitmap) and on the whole state: every
 * space's quota, charges and slots, every object's liveness, type, death,
 * references and donor; and joist_check_invariants must
 * report nothing.
 *
 *   JOIST_PROP_SEEDS=N   number of seeds to run (default 100)
 *   JOIST_PROP_STEPS=N   steps per seed (default 1500)
 *   JOIST_PROP_SEED=S    run only seed S (replay a reported failure)
 *
 * The run ends with a coverage line per operation, counting each result. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ---- deterministic RNG (xorshift64*) ---- */

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

/* ---- the shadow ---- */

#define SLOTS JOIST_CSPACE_SLOTS
#define LEAF  JOIST_CNODE_LEAF
#define NONE  UINT32_MAX

struct sh_cap {
    int present;
    uint32_t obj;
    uint8_t type;
    joist_rights rights;
    joist_badge badge;
};

struct sh_space {
    int live;
    uint32_t quota;
    uint32_t charged;
    uint32_t task_obj; /* plus one; 0 for the root and once destroyed */
    int leaf[JOIST_CNODE_ROOT];
    struct sh_cap slot[SLOTS];
};

struct sh_obj {
    int live;
    uint8_t type;
    int dead; /* frames: killed; tasks: destroyed (endpoints: derived, below) */
    uint32_t refs;
    uint32_t recv; /* endpoints: capabilities with RECV */
    uint32_t donor;
    uint32_t target; /* tasks: the space */
    uint64_t base, npages;
};

struct sh_map {
    int used;
    uint32_t space, frame;
    uint64_t va;
};

static struct sh_space S[JOIST_MAX_CSPACES];
static struct sh_obj O[JOIST_MAX_OBJECTS];
#define SH_MAPS 64u /* (no mapping is made: see the top) */
static struct sh_map M[SH_MAPS];
static uint32_t sh_live, sh_leaves;

static joist_rights sh_full(uint8_t type)
{
    switch (type) {
    case JOIST_OBJ_ENDPOINT:
    case JOIST_OBJ_NOTIFICATION:
        return (joist_rights)(JOIST_R_SEND | JOIST_R_RECV | JOIST_R_GRANT);
    case JOIST_OBJ_FRAME:
        return JOIST_R_ALL;
    case JOIST_OBJ_TASK:
    case JOIST_OBJ_REPLY:
        return (joist_rights)(JOIST_R_CONTROL | JOIST_R_GRANT);
    default:
        return 0;
    }
}

/* The capability in slot of space cs, or NULL if out of range or its leaf
 * is absent. */
static struct sh_cap *sh_slot(uint32_t cs, uint32_t slot)
{
    if (slot >= SLOTS || (slot >= LEAF && !S[cs].leaf[slot / LEAF])) {
        return NULL;
    }
    return &S[cs].slot[slot];
}

static joist_err sh_space_ok(uint32_t cs)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    return S[cs].live ? JOIST_OK : JOIST_E_DEAD;
}

/* Is obj dead, for the calls that refuse dead objects? An endpoint is dead
 * when no capability with RECV names it (I5). */
static int sh_dead(uint32_t obj)
{
    return O[obj].type == JOIST_OBJ_ENDPOINT ? O[obj].recv == 0 : O[obj].dead;
}

static void sh_unmap(uint32_t i)
{
    S[M[i].space].quota++;
    S[M[i].space].charged--;
    memset(&M[i], 0, sizeof M[i]);
}

static void sh_put(uint32_t cs, uint32_t slot, struct sh_cap c)
{
    S[cs].slot[slot] = c;
    O[c.obj].refs++;
    O[c.obj].recv += (uint32_t)(c.type == JOIST_OBJ_ENDPOINT && (c.rights & JOIST_R_RECV) != 0);
}

/* Spaces whose destruction has begun and isn't finished. */
static uint32_t work[JOIST_MAX_CSPACES];
static uint32_t nwork;

static void sh_begin_destroy(uint32_t s)
{
    if (S[s].task_obj != 0) {
        O[S[s].task_obj - 1u].dead = 1;
        S[s].task_obj = 0;
    }
    work[nwork++] = s;
}

static void sh_remove(uint32_t cs, uint32_t slot)
{
    struct sh_cap c = S[cs].slot[slot];
    memset(&S[cs].slot[slot], 0, sizeof S[cs].slot[slot]);
    struct sh_obj *o = &O[c.obj];
    o->recv -= (uint32_t)(c.type == JOIST_OBJ_ENDPOINT && (c.rights & JOIST_R_RECV) != 0);
    if (--o->refs != 0) {
        return;
    }
    if (o->type == JOIST_OBJ_FRAME) {
        for (uint32_t i = 0; i < SH_MAPS; i++) {
            if (M[i].used && M[i].frame == c.obj) {
                sh_unmap(i);
            }
        }
    }
    if (o->type == JOIST_OBJ_TASK && !o->dead) {
        sh_begin_destroy(o->target); /* unreachable now */
    }
    S[o->donor].charged--;
    if (S[o->donor].live) {
        S[o->donor].quota++;
    }
    memset(o, 0, sizeof *o);
    sh_live--;
}

/* Run every begun destruction to its end: the order differs from the
 * model's, the end state doesn't. Every task here is the root's child. */
static void sh_drain(void)
{
    while (nwork != 0) {
        uint32_t s = work[--nwork];
        for (uint32_t i = 0; i < SLOTS; i++) {
            if (S[s].slot[i].present) {
                sh_remove(s, i);
            }
        }
        for (uint32_t l = 1; l < JOIST_CNODE_ROOT; l++) {
            if (S[s].leaf[l]) {
                S[s].leaf[l] = 0;
                S[s].charged--;
                S[s].quota++;
                sh_leaves--;
            }
        }
        for (uint32_t i = 0; i < SH_MAPS; i++) {
            if (M[i].used && M[i].space == s) {
                sh_unmap(i);
            }
        }
        S[0].quota += S[s].quota;
        for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
            if (O[o].live && O[o].donor == s) {
                O[o].donor = 0;
                S[0].charged++;
                S[s].charged--;
            }
        }
        S[s].quota = 0;
        S[s].live = 0;
    }
}

/* The lookup every capability-taking call starts with. */
static joist_err sh_cap(uint32_t cs, uint32_t slot, struct sh_cap **out)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    struct sh_cap *c = sh_slot(cs, slot);
    if (c == NULL) {
        return JOIST_E_RANGE;
    }
    if (!c->present) {
        return JOIST_E_EMPTY;
    }
    *out = c;
    return JOIST_OK;
}

/* joist__control_cap's checks. */
static joist_err sh_control(uint32_t cs, uint32_t slot, uint8_t type, uint32_t *obj)
{
    struct sh_cap *c;
    joist_err e = sh_cap(cs, slot, &c);
    if (e != JOIST_OK) {
        return e;
    }
    if (c->type != type) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & JOIST_R_CONTROL) == 0) {
        return JOIST_E_RIGHTS;
    }
    if (sh_dead(c->obj)) {
        return JOIST_E_GONE;
    }
    *obj = c->obj;
    return JOIST_OK;
}

static joist_err sh_dst(uint32_t cs, uint32_t dst)
{
    const struct sh_cap *d = sh_slot(cs, dst);
    if (d == NULL) {
        return JOIST_E_RANGE;
    }
    return d->present ? JOIST_E_OCCUPIED : JOIST_OK;
}

/* Distinct (endpoint, badge) pairs held with SEND: the no-senders accounts
 * in use (nothing is bound here, so none is ever left fired). */
static int sh_pair(uint32_t obj, joist_badge b)
{
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        for (uint32_t i = 0; i < SLOTS; i++) {
            const struct sh_cap *x = &S[c].slot[i];
            if (x->present && x->obj == obj && x->badge == b && x->badge != 0 &&
                x->type == JOIST_OBJ_ENDPOINT && (x->rights & JOIST_R_SEND) != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static uint32_t sh_accounts(void)
{
    static uint32_t po[JOIST_MAX_CSPACES * SLOTS];
    static joist_badge pb[JOIST_MAX_CSPACES * SLOTS];
    uint32_t n = 0;
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        for (uint32_t i = 0; i < SLOTS; i++) {
            const struct sh_cap *x = &S[c].slot[i];
            if (!x->present || x->badge == 0 || x->type != JOIST_OBJ_ENDPOINT ||
                (x->rights & JOIST_R_SEND) == 0) {
                continue;
            }
            uint32_t j = 0;
            while (j < n && (po[j] != x->obj || pb[j] != x->badge)) {
                j++;
            }
            if (j == n) {
                po[n] = x->obj;
                pb[n++] = x->badge;
            }
        }
    }
    return n;
}

/* ---- each operation: predict, and apply on success ---- */

static joist_err sh_obj_create_rc(uint32_t cs, uint8_t type, uint32_t dst)
{
    joist_err e = sh_space_ok(cs);
    if (e == JOIST_OK && sh_slot(cs, dst) == NULL) {
        e = JOIST_E_RANGE;
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (type != JOIST_OBJ_ENDPOINT && type != JOIST_OBJ_NOTIFICATION && type != JOIST_OBJ_REPLY) {
        return JOIST_E_TYPE;
    }
    if (S[cs].slot[dst].present) {
        return JOIST_E_OCCUPIED;
    }
    return S[cs].quota == 0 || sh_live == JOIST_MAX_OBJECTS ? JOIST_E_NOMEM : JOIST_OK;
}

/* A new object at index idx (the model's choice, checked free), charged to
 * cs, with a capability in dst. */
static int sh_new(uint32_t idx, uint32_t cs, uint32_t dst, uint8_t type, joist_rights rights)
{
    if (idx >= JOIST_MAX_OBJECTS || O[idx].live) {
        return 0;
    }
    O[idx] = (struct sh_obj){.live = 1, .type = type, .donor = cs};
    sh_live++;
    S[cs].quota--;
    S[cs].charged++;
    sh_put(cs, dst, (struct sh_cap){.present = 1, .obj = idx, .type = type, .rights = rights});
    return 1;
}

static joist_err sh_copy(uint32_t cs, uint32_t src, uint32_t dst, joist_rights mask, int mint,
                         joist_badge badge)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    struct sh_cap *from = sh_slot(cs, src);
    struct sh_cap *d = sh_slot(cs, dst);
    if (from == NULL || d == NULL) {
        return JOIST_E_RANGE;
    }
    if (!from->present) {
        return JOIST_E_EMPTY;
    }
    if (d->present) {
        return JOIST_E_OCCUPIED;
    }
    if (mint) {
        if ((from->rights & JOIST_R_GRANT) == 0) {
            return JOIST_E_RIGHTS;
        }
        if (from->badge != 0 || badge == 0) {
            return JOIST_E_BADGE;
        }
        if (from->type == JOIST_OBJ_ENDPOINT && (from->rights & mask & JOIST_R_SEND) != 0 &&
            !sh_pair(from->obj, badge) && sh_accounts() >= JOIST_MAX_BADGE_ACCTS) {
            return JOIST_E_NOMEM;
        }
    }
    struct sh_cap to = *from;
    to.rights = (joist_rights)(from->rights & mask);
    if (mint) {
        to.badge = badge;
    }
    sh_put(cs, dst, to);
    return JOIST_OK;
}

static joist_err sh_delete(uint32_t cs, uint32_t slot)
{
    struct sh_cap *c;
    joist_err e = sh_cap(cs, slot, &c);
    if (e != JOIST_OK) {
        return e;
    }
    sh_remove(cs, slot);
    sh_drain();
    return JOIST_OK;
}

static joist_err sh_task_create_rc(uint32_t cs, uint32_t donation, uint32_t dst, uint32_t *ns)
{
    joist_err e = sh_space_ok(cs);
    if (e == JOIST_OK) {
        e = sh_dst(cs, dst);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (S[cs].quota < donation + 1u) {
        return JOIST_E_NOMEM;
    }
    for (uint32_t i = 0; i < JOIST_MAX_CSPACES; i++) {
        if (!S[i].live && S[i].charged == 0) {
            *ns = i;
            return sh_live < JOIST_MAX_OBJECTS ? JOIST_OK : JOIST_E_NOMEM;
        }
    }
    return JOIST_E_NOMEM;
}

static joist_err sh_task_destroy(uint32_t cs, uint32_t slot)
{
    uint32_t obj;
    joist_err e = sh_control(cs, slot, JOIST_OBJ_TASK, &obj);
    if (e != JOIST_OK) {
        return e;
    }
    sh_begin_destroy(O[obj].target);
    sh_drain();
    return JOIST_OK;
}

static joist_err sh_grow(uint32_t cs, uint32_t l)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (l == 0 || l >= JOIST_CNODE_ROOT) {
        return JOIST_E_RANGE;
    }
    if (S[cs].leaf[l]) {
        return JOIST_E_OCCUPIED;
    }
    if (S[cs].quota == 0 || sh_leaves == JOIST_MAX_LEAVES) {
        return JOIST_E_NOMEM;
    }
    S[cs].leaf[l] = 1;
    S[cs].quota--;
    S[cs].charged++;
    sh_leaves++;
    return JOIST_OK;
}

static joist_err sh_shrink(uint32_t cs, uint32_t l)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (l == 0 || l >= JOIST_CNODE_ROOT || !S[cs].leaf[l]) {
        return JOIST_E_RANGE;
    }
    for (uint32_t i = l * LEAF; i < (l + 1u) * LEAF; i++) {
        if (S[cs].slot[i].present) {
            return JOIST_E_STATE;
        }
    }
    S[cs].leaf[l] = 0;
    S[cs].quota++;
    S[cs].charged--;
    sh_leaves--;
    return JOIST_OK;
}

static int sh_mapped(uint32_t frame)
{
    for (uint32_t i = 0; i < SH_MAPS; i++) {
        if (M[i].used && M[i].frame == frame) {
            return 1;
        }
    }
    return 0;
}

static joist_err sh_split_rc(uint32_t cs, uint32_t slot, uint64_t at, uint32_t dst)
{
    uint32_t f;
    joist_err e = sh_control(cs, slot, JOIST_OBJ_FRAME, &f);
    if (e != JOIST_OK) {
        return e;
    }
    if (at == 0 || at >= O[f].npages) {
        return JOIST_E_RANGE;
    }
    e = sh_dst(cs, dst);
    if (e != JOIST_OK) {
        return e;
    }
    if (O[f].refs != 1 || sh_mapped(f)) {
        return JOIST_E_STATE;
    }
    return S[cs].quota == 0 || sh_live == JOIST_MAX_OBJECTS ? JOIST_E_NOMEM : JOIST_OK;
}

/* 0022: obj_kill. */
static joist_err sh_kill(uint32_t cs, uint32_t slot)
{
    struct sh_cap *c;
    joist_err e = sh_cap(cs, slot, &c);
    if (e != JOIST_OK) {
        return e;
    }
    if (c->type != JOIST_OBJ_FRAME) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & JOIST_R_KILL) == 0) {
        return JOIST_E_RIGHTS;
    }
    if (O[c->obj].dead) {
        return JOIST_E_GONE;
    }
    O[c->obj].dead = 1;
    for (uint32_t i = 0; i < SH_MAPS; i++) {
        if (M[i].used && M[i].frame == c->obj) {
            sh_unmap(i);
        }
    }
    return JOIST_OK;
}

/* 0022: cap_copy_batch, as the decision states it. */
static joist_err sh_batch(uint32_t cs, uint32_t tslot, uint32_t first, uint32_t count,
                          joist_rights mask, uint32_t flags, uint32_t *done, uint32_t *skipped)
{
    *done = 0;
    *skipped = 0;
    uint32_t t;
    joist_err e = sh_control(cs, tslot, JOIST_OBJ_TASK, &t);
    if (e == JOIST_E_GONE) {
        return JOIST_E_DEAD; /* a dying or destroyed child */
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (count > 32u || first >= SLOTS || count > SLOTS - first || flags > 1u) {
        return JOIST_E_RANGE;
    }
    uint32_t child = O[t].target;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t s = first + i;
        struct sh_cap *c = sh_slot(cs, s);
        if (c == NULL || !c->present) {
            continue;
        }
        joist_rights r = (joist_rights)(c->rights & mask);
        if ((c->rights & JOIST_R_GRANT) == 0 ||
            (c->type == JOIST_OBJ_ENDPOINT && (r & JOIST_R_RECV) != 0 && flags == 0)) {
            *skipped |= 1u << i;
            continue;
        }
        e = sh_dst(child, s);
        if (e != JOIST_OK) {
            *done = i;
            return e;
        }
        struct sh_cap copy = *c;
        copy.rights = r;
        sh_put(child, s, copy);
    }
    *done = count;
    return JOIST_OK;
}

/* ---- comparison ---- */

static struct joist_kernel k;
static char why[256];

static int compare_all(void)
{
    if (k.objects_live != sh_live) {
        (void)snprintf(why, sizeof why, "objects_live %u, shadow %u", k.objects_live, sh_live);
        return 0;
    }
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        const struct joist_cspace_state *r = &k.cs[c];
        const struct sh_space *s = &S[c];
        if ((r->live != 0) != (s->live != 0) || r->quota != s->quota || r->charged != s->charged ||
            r->task_obj != s->task_obj) {
            (void)snprintf(why, sizeof why,
                           "space %u: live %u/%d quota %u/%u charged %u/%u task %u/%u", c, r->live,
                           s->live, r->quota, s->quota, r->charged, s->charged, r->task_obj,
                           s->task_obj);
            return 0;
        }
        for (uint32_t l = 1; l < JOIST_CNODE_ROOT; l++) {
            if ((r->leaf[l] != 0) != (s->leaf[l] != 0)) {
                (void)snprintf(why, sizeof why, "space %u leaf %u: presence differs", c, l);
                return 0;
            }
        }
        for (uint32_t i = 0; i < SLOTS; i++) {
            const struct joist_cap *rc = joist__slot_c(&k, c, i);
            const struct sh_cap *sc = &s->slot[i];
            int rp = rc != NULL && rc->obj != 0;
            if (rp != (sc->present != 0)) {
                (void)snprintf(why, sizeof why, "space %u slot %u: presence %d/%d", c, i, rp,
                               sc->present);
                return 0;
            }
            if (rp && (rc->obj - 1u != sc->obj || rc->type != sc->type ||
                       rc->rights != sc->rights || rc->badge != sc->badge)) {
                (void)snprintf(why, sizeof why,
                               "space %u slot %u: obj %u/%u type %u/%u rights 0x%x/0x%x "
                               "badge %" PRIu64 "/%" PRIu64,
                               c, i, rc->obj - 1u, sc->obj, rc->type, sc->type, rc->rights,
                               sc->rights, rc->badge, sc->badge);
                return 0;
            }
        }
    }
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *r = &k.obj[o];
        const struct sh_obj *s = &O[o];
        int rlive = r->type != JOIST_OBJ_NONE;
        if (rlive != s->live) {
            (void)snprintf(why, sizeof why, "object %u: live %d/%d", o, rlive, s->live);
            return 0;
        }
        if (rlive && (r->type != s->type || r->refs != s->refs || r->donor != s->donor ||
                      (r->dead != 0) != (sh_dead(o) != 0))) {
            (void)snprintf(why, sizeof why,
                           "object %u: type %u/%u refs %u/%u donor %u/%u dead %u/%d", o, r->type,
                           s->type, r->refs, s->refs, r->donor, s->donor, r->dead, sh_dead(o));
            return 0;
        }
        if (rlive && r->type == JOIST_OBJ_FRAME && (r->base != s->base || r->npages != s->npages)) {
            (void)snprintf(why, sizeof why,
                           "frame %u: pages %" PRIu64 "+%" PRIu64 ", shadow %" PRIu64 "+%" PRIu64,
                           o, r->base, r->npages, s->base, s->npages);
            return 0;
        }
    }
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        if (k.obj[o].type == JOIST_OBJ_FRAME && k.obj[o].mapcount != 0) {
            (void)snprintf(why, sizeof why, "frame %u mapped, but nothing maps here", o);
            return 0;
        }
    }
    return joist_check_invariants(&k, why, sizeof why) == 0;
}

/* ---- choices: biased towards what can succeed ---- */

/* A live space: the root half the time. */
static uint32_t pick_space(void)
{
    if (pick(16) == 0) {
        return pick(JOIST_MAX_CSPACES + 1u);
    }
    if (pick(2) == 0) {
        return 0;
    }
    uint32_t start = pick(JOIST_MAX_CSPACES);
    for (uint32_t n = 0; n < JOIST_MAX_CSPACES; n++) {
        uint32_t c = (start + n) % JOIST_MAX_CSPACES;
        if (S[c].live) {
            return c;
        }
    }
    return 0;
}

/* An occupied slot of type (0: any) 3 times in 4, when there is one;
 * of those, one naming a live object 3 times in 4. */
static uint32_t pick_slot(uint32_t cs, uint8_t type)
{
    if (cs < JOIST_MAX_CSPACES && pick(4) != 0) {
        int alive = pick(4) != 0;
        for (int pass = 0; pass < 2; pass++, alive = 0) {
            uint32_t start = pick(SLOTS);
            for (uint32_t n = 0; n < SLOTS; n++) {
                uint32_t i = (start + n) % SLOTS;
                const struct sh_cap *c = &S[cs].slot[i];
                if (c->present && (type == 0 || c->type == type) && (!alive || !sh_dead(c->obj))) {
                    return i;
                }
            }
        }
    }
    return pick(SLOTS + 1u);
}

/* A free slot 3 times in 4, mostly in leaf 0. */
static uint32_t pick_dst(uint32_t cs)
{
    if (cs < JOIST_MAX_CSPACES && pick(4) != 0) {
        uint32_t span = pick(4) == 0 ? SLOTS : LEAF;
        uint32_t start = pick(span);
        for (uint32_t n = 0; n < span; n++) {
            uint32_t i = (start + n) % span;
            if (sh_slot(cs, i) != NULL && !S[cs].slot[i].present) {
                return i;
            }
        }
    }
    return pick(SLOTS + 1u);
}

static joist_rights pick_mask(void)
{
    switch (pick(4)) {
    case 0:
        return (joist_rights)pick(16);
    case 1:
        return (joist_rights)~JOIST_R_KILL;
    default:
        return JOIST_R_ALL;
    }
}

/* ---- coverage ---- */

enum {
    OP_CREATE,
    OP_DERIVE,
    OP_MINT,
    OP_DELETE,
    OP_TASK_CREATE,
    OP_TASK_DESTROY,
    OP_GROW,
    OP_SHRINK,
    OP_SPLIT,
    OP_KILL,
    OP_BATCH,
    OP_BOOT_FRAME,
    OP_COUNT
};
static const char *const op_name[OP_COUNT] = {
    "obj_create", "derive", "mint", "delete", "task_create", "task_destroy", "grow",
    "shrink",     "split",  "obj_kill",    "copy_batch",   "boot_frame"};
#define N_ERR 15u
static unsigned long coverage[OP_COUNT][N_ERR];
static unsigned long batch_copied, batch_skipped, batch_stopped;

static unsigned long env_ulong(const char *name, unsigned long def)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? strtoul(v, NULL, 0) : def;
}

static int start(uint64_t seed)
{
    rng_state = seed * 0x9E3779B97F4A7C15ULL + 1u;
    joist_init(&k);
    memset(S, 0, sizeof S);
    memset(O, 0, sizeof O);
    memset(M, 0, sizeof M);
    sh_live = 0;
    sh_leaves = 0;
    nwork = 0;
    uint32_t budget = 24u + pick(80);
    joist_cspace root;
    if (joist_boot(&k, budget, &root) != JOIST_OK || root != 0) {
        return 0;
    }
    S[0].live = 1;
    S[0].quota = budget;
    /* Memory, as at boot: three frames of up to 32 pages. */
    static const uint64_t base[3] = {0x100, 0x1000, 0x2000};
    for (uint32_t i = 0; i < 3; i++) {
        uint64_t n = 1u + pick(32);
        if (joist_boot_frame(&k, 0, 10 + i, base[i], n) != JOIST_OK) {
            return 0;
        }
        uint32_t idx = k.cs[0].slot[10 + i].obj - 1u;
        if (!sh_new(idx, 0, 10 + i, JOIST_OBJ_FRAME, JOIST_R_ALL)) {
            return 0;
        }
        O[idx].base = base[i];
        O[idx].npages = n;
    }
    return 1;
}

static int run_seed(uint64_t seed, unsigned long steps)
{
    if (!start(seed)) {
        fprintf(stderr, "  seed %" PRIu64 ": the starting world could not be made\n", seed);
        return 0;
    }
    for (unsigned long step = 0; step < steps; step++) {
        uint32_t roll = pick(100);
        uint32_t cs = pick_space();
        joist_err got = JOIST_OK;
        joist_err want = JOIST_OK;
        uint32_t got_done = 0, got_skip = 0, want_done = 0, want_skip = 0;
        int op;
        why[0] = '\0';

        if (roll < 9) {
            op = OP_CREATE;
            uint8_t type = pick(8) == 0 ? (uint8_t)pick(8) : (uint8_t)(1u + pick(2));
            uint32_t d = pick_dst(cs);
            want = sh_obj_create_rc(cs, type, d);
            got = joist_obj_create(&k, cs, (joist_obj_type)type, d);
            if (got == JOIST_OK && want == JOIST_OK &&
                !sh_new(joist__slot_c(&k, cs, d)->obj - 1u, cs, d, type, sh_full(type))) {
                uint32_t ri = joist__slot_c(&k, cs, d)->obj - 1u;
                (void)snprintf(why, sizeof why,
                               "obj_create reused live object %u (type %u refs %u donor %u)", ri,
                               O[ri].type, O[ri].refs, O[ri].donor);
                got = JOIST_E_STATE;
            }
        } else if (roll < 21) {
            op = OP_DERIVE;
            uint32_t s = pick_slot(cs, 0);
            uint32_t d = pick_dst(cs);
            joist_rights m = pick_mask();
            want = sh_copy(cs, s, d, m, 0, 0);
            got = joist_cap_derive(&k, cs, s, d, m);
        } else if (roll < 27) {
            op = OP_MINT;
            uint32_t s = pick_slot(cs, pick(4) != 0 ? (uint8_t)JOIST_OBJ_ENDPOINT : 0);
            uint32_t d = pick_dst(cs);
            joist_rights m = pick_mask();
            joist_badge b = pick(8) == 0 ? 0 : 1u + pick(6);
            want = sh_copy(cs, s, d, m, 1, b);
            got = joist_cap_mint(&k, cs, s, d, m, b);
        } else if (roll < 37) {
            op = OP_DELETE;
            uint32_t s = pick_slot(cs, 0);
            want = sh_delete(cs, s);
            got = joist_cap_delete(&k, cs, s);
        } else if (roll < 43) {
            op = OP_TASK_CREATE;
            uint32_t donation = pick(6);
            uint32_t d = pick_dst(0);
            uint32_t ns = NONE;
            want = sh_task_create_rc(0, donation, d, &ns);
            got = joist_task_create(&k, 0, donation, d);
            if (got == JOIST_OK && want == JOIST_OK) {
                uint32_t idx = joist__slot_c(&k, 0, d)->obj - 1u;
                if (k.obj[idx].target != ns + 1u ||
                    !sh_new(idx, 0, d, JOIST_OBJ_TASK, sh_full(JOIST_OBJ_TASK))) {
                    (void)snprintf(why, sizeof why, "task_create: space %u, shadow %u",
                                   k.obj[idx].target - 1u, ns);
                    got = JOIST_E_STATE;
                } else {
                    O[idx].target = ns;
                    memset(&S[ns], 0, sizeof S[ns]);
                    S[ns].live = 1;
                    S[ns].quota = donation;
                    S[ns].task_obj = idx + 1u;
                    S[0].quota -= donation;
                }
            }
        } else if (roll < 46) {
            op = OP_TASK_DESTROY;
            uint32_t s = pick_slot(0, JOIST_OBJ_TASK);
            want = sh_task_destroy(0, s);
            got = joist_task_destroy(&k, 0, s);
        } else if (roll < 50) {
            uint32_t l = pick(4) != 0 ? 1u : pick(JOIST_CNODE_ROOT + 1u);
            op = pick(3) != 0 ? OP_GROW : OP_SHRINK;
            want = op == OP_GROW ? sh_grow(cs, l) : sh_shrink(cs, l);
            got = op == OP_GROW ? joist_cspace_grow(&k, cs, l) : joist_cspace_shrink(&k, cs, l);
        } else if (roll < 54) {
            op = OP_SPLIT;
            uint32_t s = pick_slot(cs, JOIST_OBJ_FRAME);
            uint32_t d = pick_dst(cs);
            const struct sh_cap *c = cs < JOIST_MAX_CSPACES ? sh_slot(cs, s) : NULL;
            uint64_t n =
                c != NULL && c->present && c->type == JOIST_OBJ_FRAME ? O[c->obj].npages : 4u;
            uint64_t at = pick((uint32_t)n + 2u);
            want = sh_split_rc(cs, s, at, d);
            got = joist_frame_split(&k, cs, s, at, d);
            if (got == JOIST_OK && want == JOIST_OK) {
                uint32_t f = S[cs].slot[s].obj;
                uint32_t idx = joist__slot_c(&k, cs, d)->obj - 1u;
                if (!sh_new(idx, cs, d, JOIST_OBJ_FRAME, S[cs].slot[s].rights)) {
                    (void)snprintf(why, sizeof why, "frame_split reused a live object");
                    got = JOIST_E_STATE;
                } else {
                    O[idx].base = O[f].base + at;
                    O[idx].npages = O[f].npages - at;
                    O[f].npages = at;
                }
            }
        } else if (roll < 73) {
            /* More memory for the root now and then, at pages no frame
             * has had, as the kernel makes frames at boot. */
            op = OP_BOOT_FRAME;
            uint32_t d = pick_dst(0);
            uint64_t n = 1u + pick(32);
            uint64_t base = 0x10000u + 0x100u * step;
            want = sh_dst(0, d);
            if (want == JOIST_OK && (S[0].quota == 0 || sh_live == JOIST_MAX_OBJECTS)) {
                want = JOIST_E_NOMEM;
            }
            got = joist_boot_frame(&k, 0, d, base, n);
            if (got == JOIST_OK && want == JOIST_OK) {
                uint32_t idx = joist__slot_c(&k, 0, d)->obj - 1u;
                if (!sh_new(idx, 0, d, JOIST_OBJ_FRAME, JOIST_R_ALL)) {
                    (void)snprintf(why, sizeof why, "boot_frame reused a live object");
                    got = JOIST_E_STATE;
                } else {
                    O[idx].base = base;
                    O[idx].npages = n;
                }
            }
        } else if (roll < 77) {
            op = OP_KILL;
            uint32_t s = pick_slot(cs, pick(4) != 0 ? (uint8_t)JOIST_OBJ_FRAME : 0);
            want = sh_kill(cs, s);
            got = joist_obj_kill(&k, cs, s);
        } else {
            op = OP_BATCH;
            uint32_t t = pick_slot(cs, JOIST_OBJ_TASK);
            uint32_t first = pick(8) == 0 ? pick(SLOTS + 2u) : pick_slot(cs, 0);
            if (first < SLOTS && pick(2) == 0) {
                first = first >= 4u ? first - pick(4) : 0;
            }
            uint32_t count = pick(8) == 0 ? 33u + pick(8) : pick(33);
            joist_rights m = pick_mask();
            uint32_t flags = pick(16) == 0 ? 1u + pick(4) : (uint32_t)(pick(3) == 0);
            want = sh_batch(cs, t, first, count, m, flags, &want_done, &want_skip);
            got = joist_cap_copy_batch(&k, cs, t, first, count, m, flags, &got_done, &got_skip);
            if (got == want && (got_done != want_done || got_skip != want_skip)) {
                (void)snprintf(why, sizeof why, "done %u/%u skipped 0x%x/0x%x (model/shadow)",
                               got_done, want_done, got_skip, want_skip);
                got = JOIST_BLOCKED;
            }
            batch_copied += (unsigned long)(got_done != 0);
            batch_skipped += (unsigned long)(got_skip != 0);
            batch_stopped +=
                (unsigned long)(got == JOIST_E_OCCUPIED || (got == JOIST_E_RANGE && got_done != 0));
        }

        coverage[op][(unsigned)got < N_ERR ? (unsigned)got : N_ERR - 1u]++;
        if (got != want) {
            fprintf(stderr, "  seed %" PRIu64 " step %lu: %s returned %s, shadow predicted %s %s\n",
                    seed, step, op_name[op], joist_err_name(got), joist_err_name(want), why);
            return 0;
        }
        if (!compare_all()) {
            fprintf(stderr, "  seed %" PRIu64 " step %lu after %s: %s\n", seed, step, op_name[op],
                    why);
            return 0;
        }
    }
    return 1;
}

JT_TEST(props_kill_copy_model_matches_shadow)
{
    unsigned long steps = env_ulong("JOIST_PROP_STEPS", 1500);
    const char *one = getenv("JOIST_PROP_SEED");
    memset(coverage, 0, sizeof coverage);
    batch_copied = batch_skipped = batch_stopped = 0;

    if (one != NULL && *one != '\0') {
        JT_CHECK(run_seed(strtoull(one, NULL, 0), steps));
    } else {
        unsigned long seeds = env_ulong("JOIST_PROP_SEEDS", 100);
        for (unsigned long s = 1; s <= seeds; s++) {
            if (!run_seed(s, steps)) {
                JT_CHECK(0 && "property failed; replay with JOIST_PROP_SEED");
                break;
            }
        }
    }
    for (int op = 0; op < OP_COUNT; op++) {
        fprintf(stderr, "  coverage %-13s", op_name[op]);
        for (unsigned e = 0; e < N_ERR; e++) {
            if (coverage[op][e] != 0) {
                fprintf(stderr, " %s=%lu", joist_err_name((joist_err)e), coverage[op][e]);
            }
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr,
            "  coverage 0022: %lu batches copied something, %lu skipped some, %lu stopped part "
            "way\n",
            batch_copied, batch_skipped, batch_stopped);
}
