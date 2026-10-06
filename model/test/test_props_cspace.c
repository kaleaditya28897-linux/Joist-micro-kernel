/* test_props_cspace.c - property test for capability spaces (Phase 1 work
 * item 1; invariants I1, I3, I7, I8).
 *
 * A seeded generator issues random operations, valid and invalid, against the
 * model and against an independent shadow model written as plainly as
 * possible. After every step the two must agree on the return code and on the
 * entire state, and joist_check_invariants must report nothing.
 *
 *   JOIST_PROP_SEEDS=N   number of seeds to run (default 200)
 *   JOIST_PROP_STEPS=N   steps per seed (default 2000)
 *   JOIST_PROP_SEED=S    run only seed S (replay a reported failure)
 *
 * The run ends with a coverage line per operation, counting each result code,
 * so that a generator which never reaches an error path is visible. */
#include "harness.h"
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

/* ---- the shadow model ---- */

struct sh_cap {
    int present;
    uint32_t obj; /* real object index */
    uint8_t type;
    joist_rights rights;
    joist_badge badge;
};

struct sh_space {
    int live;
    uint32_t quota;
    uint32_t charged;
    struct sh_cap slot[JOIST_CSPACE_SLOTS];
};

struct sh_obj {
    int live;
    uint8_t type;
    uint32_t donor;
};

static struct sh_space sh_cs[JOIST_MAX_CSPACES];
static struct sh_obj sh_ob[JOIST_MAX_OBJECTS];
static uint32_t sh_live;

static joist_rights sh_full(uint8_t type)
{
    return (type == JOIST_OBJ_ENDPOINT || type == JOIST_OBJ_NOTIFICATION)
               ? (joist_rights)(JOIST_R_SEND | JOIST_R_RECV | JOIST_R_GRANT)
               : 0;
}

static uint32_t sh_refs(uint32_t obj)
{
    uint32_t n = 0;
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
            if (sh_cs[c].slot[i].present && sh_cs[c].slot[i].obj == obj) {
                n++;
            }
        }
    }
    return n;
}

static void sh_drop(uint32_t obj)
{
    if (sh_refs(obj) == 0) {
        struct sh_space *d = &sh_cs[sh_ob[obj].donor];
        d->charged--;
        if (d->live) {
            d->quota++;
        }
        sh_ob[obj].live = 0;
        sh_live--;
    }
}

/* Each sh_* operation predicts the result code and applies the effect. */

static joist_err sh_space_create(uint32_t quota, joist_cspace *out)
{
    for (uint32_t i = 0; i < JOIST_MAX_CSPACES; i++) {
        if (!sh_cs[i].live && sh_cs[i].charged == 0) {
            memset(&sh_cs[i], 0, sizeof sh_cs[i]);
            sh_cs[i].live = 1;
            sh_cs[i].quota = quota;
            *out = i;
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

static joist_err sh_space_ok(uint32_t cs)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    return sh_cs[cs].live ? JOIST_OK : JOIST_E_DEAD;
}

static joist_err sh_delete(uint32_t cs, uint32_t slot)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    struct sh_cap *c = &sh_cs[cs].slot[slot];
    if (!c->present) {
        return JOIST_E_EMPTY;
    }
    uint32_t obj = c->obj;
    memset(c, 0, sizeof *c);
    sh_drop(obj);
    return JOIST_OK;
}

static joist_err sh_space_destroy(uint32_t cs)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
        if (sh_cs[cs].slot[i].present) {
            (void)sh_delete(cs, i);
        }
    }
    sh_cs[cs].live = 0;
    sh_cs[cs].quota = 0;
    return JOIST_OK;
}

/* Object creation: the shadow can't know which table index the model picks,
 * so it predicts only the result code; the effect is applied afterwards from
 * the index the model actually used (checked to have been free). */
static joist_err sh_create_rc(uint32_t cs, uint8_t type, uint32_t dst)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (dst >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    if (sh_full(type) == 0) {
        return JOIST_E_TYPE;
    }
    if (sh_cs[cs].slot[dst].present) {
        return JOIST_E_OCCUPIED;
    }
    if (sh_cs[cs].quota == 0) {
        return JOIST_E_NOMEM;
    }
    return sh_live < JOIST_MAX_OBJECTS ? JOIST_OK : JOIST_E_NOMEM;
}

/* Does some shadow capability already hold SEND with this (object, badge)?
 * And how many distinct such pairs exist? Together they predict whether mint
 * needs a new no-senders account and whether the table is full (I4). */
static int sh_pair_exists(uint32_t obj, joist_badge badge)
{
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
            const struct sh_cap *x = &sh_cs[c].slot[i];
            if (x->present && x->obj == obj && x->badge == badge && x->badge != 0 &&
                x->type == JOIST_OBJ_ENDPOINT && (x->rights & JOIST_R_SEND) != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static uint32_t sh_pair_count(void)
{
    uint32_t n = 0;
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
            const struct sh_cap *x = &sh_cs[c].slot[i];
            if (!x->present || x->badge == 0 || x->type != JOIST_OBJ_ENDPOINT ||
                (x->rights & JOIST_R_SEND) == 0) {
                continue;
            }
            /* count each (obj, badge) pair once: at its first occurrence */
            int first = 1;
            for (uint32_t c2 = 0; c2 <= c && first; c2++) {
                for (uint32_t i2 = 0; i2 < JOIST_CSPACE_SLOTS; i2++) {
                    if (c2 == c && i2 >= i) {
                        break;
                    }
                    const struct sh_cap *y = &sh_cs[c2].slot[i2];
                    if (y->present && y->obj == x->obj && y->badge == x->badge &&
                        y->type == JOIST_OBJ_ENDPOINT && (y->rights & JOIST_R_SEND) != 0) {
                        first = 0;
                        break;
                    }
                }
            }
            n += (uint32_t)first;
        }
    }
    return n;
}

static joist_err sh_copy(uint32_t cs, uint32_t src, uint32_t dst, joist_rights mask, int mint,
                         joist_badge badge)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (src >= JOIST_CSPACE_SLOTS || dst >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    struct sh_cap *from = &sh_cs[cs].slot[src];
    if (!from->present) {
        return JOIST_E_EMPTY;
    }
    if (sh_cs[cs].slot[dst].present) {
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
            !sh_pair_exists(from->obj, badge) && sh_pair_count() >= JOIST_MAX_BADGE_ACCTS) {
            return JOIST_E_NOMEM;
        }
    }
    struct sh_cap to = *from;
    to.rights = (joist_rights)(from->rights & mask);
    if (mint) {
        to.badge = badge;
    }
    sh_cs[cs].slot[dst] = to;
    return JOIST_OK;
}

/* Pick a slot in cs, occupied with probability 3/4 when the space has any
 * occupied slot, so the run reaches deep states (many capabilities to one
 * object) rather than mostly testing argument checks. */
static uint32_t pick_slot(uint32_t cs)
{
    if (cs < JOIST_MAX_CSPACES && pick(4) != 0) {
        uint32_t start = pick(JOIST_CSPACE_SLOTS);
        for (uint32_t n = 0; n < JOIST_CSPACE_SLOTS; n++) {
            uint32_t i = (start + n) % JOIST_CSPACE_SLOTS;
            if (sh_cs[cs].slot[i].present) {
                return i;
            }
        }
    }
    return pick(JOIST_CSPACE_SLOTS + 1u);
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
        const struct sh_space *s = &sh_cs[c];
        if ((r->live != 0) != (s->live != 0) || r->quota != s->quota || r->charged != s->charged) {
            (void)snprintf(why, sizeof why,
                           "space %u: live %u/%d quota %u/%u charged %u/%u (model/shadow)", c,
                           r->live, s->live, r->quota, s->quota, r->charged, s->charged);
            return 0;
        }
        for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
            const struct joist_cap *rc = &r->slot[i];
            const struct sh_cap *sc = &s->slot[i];
            if ((rc->obj != 0) != (sc->present != 0)) {
                (void)snprintf(why, sizeof why, "space %u slot %u: presence differs", c, i);
                return 0;
            }
            if (rc->obj != 0 && (rc->obj - 1u != sc->obj || rc->type != sc->type ||
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
        int rlive = k.obj[o].type != JOIST_OBJ_NONE;
        if (rlive != sh_ob[o].live) {
            (void)snprintf(why, sizeof why, "object %u: live %d/%d", o, rlive, sh_ob[o].live);
            return 0;
        }
        if (rlive && k.obj[o].refs != sh_refs(o)) {
            (void)snprintf(why, sizeof why, "object %u: refs %u, shadow %u", o, k.obj[o].refs,
                           sh_refs(o));
            return 0;
        }
    }
    unsigned n = joist_check_invariants(&k, why, sizeof why);
    return n == 0;
}

/* ---- coverage ---- */

enum { OP_SPACE_CREATE, OP_SPACE_DESTROY, OP_CREATE, OP_DERIVE, OP_MINT, OP_DELETE, OP_COUNT };
static const char *const op_name[OP_COUNT] = {"space_create", "space_destroy", "obj_create",
                                              "derive",       "mint",          "delete"};
#define N_ERR 9u
static unsigned long coverage[OP_COUNT][N_ERR];

/* ---- the run ---- */

static unsigned long env_ulong(const char *name, unsigned long def)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? strtoul(v, NULL, 0) : def;
}

static int run_seed(uint64_t seed, unsigned long steps)
{
    rng_state = seed * 0x9E3779B97F4A7C15ULL + 1u;
    joist_init(&k);
    memset(sh_cs, 0, sizeof sh_cs);
    memset(sh_ob, 0, sizeof sh_ob);
    sh_live = 0;

    for (unsigned long step = 0; step < steps; step++) {
        uint32_t roll = pick(100);
        /* Indexes deliberately run one past the end to reach E_RANGE. */
        uint32_t cs = pick(JOIST_MAX_CSPACES + 1u);
        uint32_t a = pick_slot(cs);                 /* source: usually occupied */
        uint32_t b = pick(JOIST_CSPACE_SLOTS + 1u); /* destination: anywhere */
        joist_rights mask = (joist_rights)pick(16);
        joist_badge badge = pick(4) == 0 ? 0 : (joist_badge)(rng() | 1u);
        int op;
        joist_err got;
        joist_err want;

        if (roll < 5) {
            op = OP_SPACE_CREATE;
            uint32_t quota = pick(40);
            joist_cspace rc_out = 0;
            joist_cspace sh_out = 0;
            got = joist_cspace_create(&k, quota, &rc_out);
            want = sh_space_create(quota, &sh_out);
            if (got == JOIST_OK && want == JOIST_OK && rc_out != sh_out) {
                (void)snprintf(why, sizeof why, "space_create picked %u, shadow %u", rc_out,
                               sh_out);
                got = JOIST_E_RANGE;
            }
        } else if (roll < 8) {
            op = OP_SPACE_DESTROY;
            got = joist_cspace_destroy(&k, cs);
            want = sh_space_destroy(cs);
        } else if (roll < 33) {
            op = OP_CREATE;
            /* JOIST_OBJ_NONE (invalid) 1 time in 16. */
            uint8_t type = pick(16) == 0 ? (uint8_t)JOIST_OBJ_NONE : (uint8_t)(1u + pick(2));
            a = b; /* create into a slot chosen anywhere, not biased to occupied */
            want = sh_create_rc(cs, type, a);
            got = joist_obj_create(&k, cs, (joist_obj_type)type, a);
            if (got == JOIST_OK && want == JOIST_OK) {
                uint32_t idx = k.cs[cs].slot[a].obj - 1u;
                if (sh_ob[idx].live) {
                    (void)snprintf(why, sizeof why, "obj_create reused live object %u", idx);
                    got = JOIST_E_RANGE;
                } else {
                    sh_ob[idx] = (struct sh_obj){.live = 1, .type = type, .donor = cs};
                    sh_live++;
                    sh_cs[cs].quota--;
                    sh_cs[cs].charged++;
                    sh_cs[cs].slot[a] = (struct sh_cap){
                        .present = 1, .obj = idx, .type = type, .rights = sh_full(type)};
                }
            }
        } else if (roll < 63) {
            op = OP_DERIVE;
            got = joist_cap_derive(&k, cs, a, b, mask);
            want = sh_copy(cs, a, b, mask, 0, 0);
        } else if (roll < 75) {
            op = OP_MINT;
            got = joist_cap_mint(&k, cs, a, b, mask, badge);
            want = sh_copy(cs, a, b, mask, 1, badge);
        } else {
            op = OP_DELETE;
            got = joist_cap_delete(&k, cs, a);
            want = sh_delete(cs, a);
        }

        coverage[op][(unsigned)got < N_ERR ? (unsigned)got : N_ERR - 1u]++;

        if (got != want) {
            fprintf(stderr, "  seed %" PRIu64 " step %lu: %s returned %s, shadow predicted %s %s\n",
                    seed, step, op_name[op], joist_err_name(got), joist_err_name(want), why);
            return 0;
        }
        /* I8, directly: a derived or minted capability never has more rights
         * than its source. */
        if ((op == OP_DERIVE || op == OP_MINT) && got == JOIST_OK &&
            (k.cs[cs].slot[b].rights & (joist_rights)~k.cs[cs].slot[a].rights) != 0) {
            fprintf(stderr, "  seed %" PRIu64 " step %lu: I8: rights grew 0x%x -> 0x%x\n", seed,
                    step, k.cs[cs].slot[a].rights, k.cs[cs].slot[b].rights);
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

JT_TEST(props_cspace_model_matches_shadow)
{
    unsigned long steps = env_ulong("JOIST_PROP_STEPS", 2000);
    const char *one = getenv("JOIST_PROP_SEED");
    memset(coverage, 0, sizeof coverage);

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
        fprintf(stderr, "  coverage %-13s", op_name[op]);
        for (unsigned e = 0; e < N_ERR; e++) {
            if (coverage[op][e] != 0) {
                fprintf(stderr, " %s=%lu", joist_err_name((joist_err)e), coverage[op][e]);
            }
        }
        fprintf(stderr, "\n");
    }
}
