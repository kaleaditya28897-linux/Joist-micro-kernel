/* test_props_task.c - property test for tasks, threads and task destruction
 * (Phase 1 work item 4; invariants I1, I3; decisions 0004, 0010, 0013).
 *
 * A seeded generator boots the model and issues random task, thread and
 * capability operations, valid and invalid, including cross-space grants so
 * that tasks share objects, hold each other's task capabilities, and form
 * cycles. An independent shadow model predicts every result and the entire
 * state. The shadow destroys tasks by plain recursion, at the moment a task
 * becomes unreachable; the model uses a work list drained afterwards. The
 * final states must still agree, which checks that the order of a cascade
 * doesn't change where quota and charges end up.
 *
 *   JOIST_PROP_SEEDS=N   number of seeds to run (default 200)
 *   JOIST_PROP_STEPS=N   steps per seed (default 2000)
 *   JOIST_PROP_SEED=S    run only seed S (replay a reported failure) */
#include "harness.h"
#include "internal.h"
#include "joist/model.h"

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
    uint32_t obj;
    uint8_t type;
    joist_rights rights;
};

struct sh_space {
    int live;
    uint32_t quota;
    uint32_t charged;
    uint32_t parent; /* space + 1, 0 for none */
    uint32_t task;   /* task object + 1, 0 for none */
    struct sh_cap slot[JOIST_CSPACE_SLOTS];
};

struct sh_obj {
    int live;
    uint8_t type;
    uint32_t donor;
    int dead;
    uint32_t target; /* space + 1 or thread + 1 */
};

struct sh_thread {
    int live;
    uint32_t cs;
    uint32_t obj; /* thread object + 1 */
};

static struct sh_space sh_cs[JOIST_MAX_CSPACES];
static struct sh_obj sh_ob[JOIST_MAX_OBJECTS];
static struct sh_thread sh_th[JOIST_MAX_THREADS];
static uint32_t sh_live;
static uint64_t sh_budget;
static uint32_t sh_root;

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

static void sh_destroy(uint32_t s);

static void sh_kill_thread(uint32_t t)
{
    if (sh_th[t].obj != 0) {
        sh_ob[sh_th[t].obj - 1u].dead = 1;
    }
    memset(&sh_th[t], 0, sizeof sh_th[t]);
}

/* Drop a reference: free the object if it was the last, destroying a task
 * or thread that just became unreachable, recursively. */
static void sh_drop(uint32_t obj)
{
    if (sh_refs(obj) != 0) {
        return;
    }
    struct sh_obj o = sh_ob[obj];
    struct sh_space *d = &sh_cs[o.donor];
    d->charged--;
    if (d->live) {
        d->quota++;
    } else {
        sh_budget--;
    }
    memset(&sh_ob[obj], 0, sizeof sh_ob[obj]);
    sh_live--;
    if (o.type == JOIST_OBJ_TASK && !o.dead) {
        sh_cs[o.target - 1u].task = 0;
        sh_destroy(o.target - 1u);
    }
    if (o.type == JOIST_OBJ_THREAD && !o.dead) {
        sh_th[o.target - 1u].obj = 0;
        sh_kill_thread(o.target - 1u);
    }
}

static void sh_remove_cap(uint32_t cs, uint32_t slot)
{
    uint32_t obj = sh_cs[cs].slot[slot].obj;
    memset(&sh_cs[cs].slot[slot], 0, sizeof sh_cs[cs].slot[slot]);
    sh_drop(obj);
}

static void sh_destroy(uint32_t s)
{
    struct sh_space *sp = &sh_cs[s];
    if (!sp->live) {
        return;
    }
    if (sp->task != 0) {
        sh_ob[sp->task - 1u].dead = 1;
        sp->task = 0;
    }
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        if (sh_th[t].live && sh_th[t].cs == s) {
            sh_kill_thread(t);
        }
    }
    for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
        if (sp->slot[i].present) {
            sh_remove_cap(s, i);
        }
    }
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        if (sh_cs[c].live && sh_cs[c].parent == s + 1u) {
            sh_cs[c].parent = sp->parent;
        }
    }
    if (sp->parent != 0) {
        struct sh_space *p = &sh_cs[sp->parent - 1u];
        p->quota += sp->quota;
        for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
            if (sh_ob[o].live && sh_ob[o].donor == s) {
                sh_ob[o].donor = sp->parent - 1u;
                p->charged++;
                sp->charged--;
            }
        }
    } else {
        sh_budget -= sp->quota;
    }
    sp->quota = 0;
    sp->parent = 0;
    sp->live = 0;
}

static joist_err sh_space_ok(uint32_t cs)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    return sh_cs[cs].live ? JOIST_OK : JOIST_E_DEAD;
}

static joist_err sh_slot_free(uint32_t cs, uint32_t dst)
{
    if (dst >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    return sh_cs[cs].slot[dst].present ? JOIST_E_OCCUPIED : JOIST_OK;
}

static joist_err sh_control(uint32_t cs, uint32_t slot, uint8_t type)
{
    if (slot >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct sh_cap *c = &sh_cs[cs].slot[slot];
    if (!c->present) {
        return JOIST_E_EMPTY;
    }
    if (c->type != type) {
        return JOIST_E_TYPE;
    }
    if ((c->rights & JOIST_R_CONTROL) == 0) {
        return JOIST_E_RIGHTS;
    }
    return sh_ob[c->obj].dead ? JOIST_E_GONE : JOIST_OK;
}

static uint32_t sh_free_space(void)
{
    for (uint32_t i = 0; i < JOIST_MAX_CSPACES; i++) {
        if (!sh_cs[i].live && sh_cs[i].charged == 0) {
            return i;
        }
    }
    return JOIST_MAX_CSPACES;
}

static uint32_t sh_free_thread(void)
{
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        if (!sh_th[i].live) {
            return i;
        }
    }
    return JOIST_MAX_THREADS;
}

/* Result predictions. Where the model picks a table index the shadow can't
 * know (objects), the shadow predicts the code only and adopts the model's
 * index afterwards, checking that it was free. */

static joist_err sh_obj_create_rc(uint32_t cs, uint8_t type, uint32_t dst)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (dst >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    if (type != JOIST_OBJ_ENDPOINT && type != JOIST_OBJ_NOTIFICATION) {
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

static joist_err sh_task_create_rc(uint32_t cs, uint32_t donation, uint32_t dst)
{
    joist_err e = sh_space_ok(cs);
    if (e == JOIST_OK) {
        e = sh_slot_free(cs, dst);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if ((uint64_t)sh_cs[cs].quota < (uint64_t)donation + 1u) {
        return JOIST_E_NOMEM;
    }
    if (sh_free_space() == JOIST_MAX_CSPACES || sh_live == JOIST_MAX_OBJECTS) {
        return JOIST_E_NOMEM;
    }
    return JOIST_OK;
}

static joist_err sh_spawn_rc(uint32_t cs, uint32_t task_slot, uint32_t dst)
{
    joist_err e = sh_space_ok(cs);
    if (e == JOIST_OK) {
        e = sh_control(cs, task_slot, JOIST_OBJ_TASK);
    }
    if (e == JOIST_OK) {
        e = sh_slot_free(cs, dst);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (sh_cs[cs].quota == 0 || sh_free_thread() == JOIST_MAX_THREADS ||
        sh_live == JOIST_MAX_OBJECTS) {
        return JOIST_E_NOMEM;
    }
    return JOIST_OK;
}

/* Install a new object chosen by the model (index idx) in the shadow. */
static int sh_adopt(uint32_t idx, uint8_t type, uint32_t cs, uint32_t dst, uint32_t target,
                    joist_rights rights)
{
    if (idx >= JOIST_MAX_OBJECTS || sh_ob[idx].live) {
        return 0;
    }
    sh_ob[idx] = (struct sh_obj){.live = 1, .type = type, .donor = cs, .target = target};
    sh_live++;
    sh_cs[cs].quota--;
    sh_cs[cs].charged++;
    sh_cs[cs].slot[dst] = (struct sh_cap){.present = 1, .obj = idx, .type = type, .rights = rights};
    return 1;
}

static joist_err sh_task_destroy(uint32_t cs, uint32_t slot)
{
    joist_err e = sh_space_ok(cs);
    if (e == JOIST_OK) {
        e = sh_control(cs, slot, JOIST_OBJ_TASK);
    }
    if (e == JOIST_OK) {
        sh_destroy(sh_ob[sh_cs[cs].slot[slot].obj].target - 1u);
    }
    return e;
}

static joist_err sh_thread_kill(uint32_t cs, uint32_t slot)
{
    joist_err e = sh_space_ok(cs);
    if (e == JOIST_OK) {
        e = sh_control(cs, slot, JOIST_OBJ_THREAD);
    }
    if (e == JOIST_OK) {
        sh_kill_thread(sh_ob[sh_cs[cs].slot[slot].obj].target - 1u);
    }
    return e;
}

static joist_err sh_space_destroy(uint32_t cs)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (cs == sh_root) {
        return JOIST_E_STATE;
    }
    sh_destroy(cs);
    return JOIST_OK;
}

/* Parent authority (0004 amendment): written plainly, as the shadow is. */
static int sh_is_descendant(uint32_t anc, uint32_t s)
{
    for (uint32_t p = sh_cs[s].parent, n = 0; p != 0 && n < JOIST_MAX_CSPACES;
         p = sh_cs[p - 1u].parent, n++) {
        if (p - 1u == anc) {
            return 1;
        }
    }
    return 0;
}

static joist_err sh_reap(uint32_t cs, uint32_t target)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (target >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    if (!sh_cs[target].live) {
        return JOIST_E_DEAD;
    }
    if (!sh_is_descendant(cs, target)) {
        return JOIST_E_RIGHTS;
    }
    sh_destroy(target);
    return JOIST_OK;
}

static joist_err sh_descendant(uint32_t cs, uint32_t from, uint32_t *out)
{
    joist_err e = sh_space_ok(cs);
    if (e != JOIST_OK) {
        return e;
    }
    if (from > JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    for (uint32_t s = from; s < JOIST_MAX_CSPACES; s++) {
        if (sh_cs[s].live && sh_is_descendant(cs, s)) {
            *out = s;
            return JOIST_OK;
        }
    }
    return JOIST_E_EMPTY;
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
    if (!sh_cs[cs].slot[slot].present) {
        return JOIST_E_EMPTY;
    }
    sh_remove_cap(cs, slot);
    return JOIST_OK;
}

static joist_err sh_copy(uint32_t from, uint32_t src, uint32_t to, uint32_t dst, joist_rights mask,
                         int grant)
{
    joist_err e = sh_space_ok(from);
    if (e == JOIST_OK) {
        e = sh_space_ok(to);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (src >= JOIST_CSPACE_SLOTS || dst >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    const struct sh_cap *c = &sh_cs[from].slot[src];
    if (!c->present) {
        return JOIST_E_EMPTY;
    }
    if (grant && (c->rights & JOIST_R_GRANT) == 0) {
        return JOIST_E_RIGHTS;
    }
    if (sh_cs[to].slot[dst].present) {
        return JOIST_E_OCCUPIED;
    }
    struct sh_cap copy = *c;
    copy.rights = (joist_rights)(c->rights & mask);
    sh_cs[to].slot[dst] = copy;
    return JOIST_OK;
}

/* ---- generator helpers ---- */

/* A live space 7 times in 8 (dead and out-of-range ones otherwise). */
static uint32_t pick_space(void)
{
    if (pick(8) != 0) {
        uint32_t start = pick(JOIST_MAX_CSPACES);
        for (uint32_t n = 0; n < JOIST_MAX_CSPACES; n++) {
            uint32_t i = (start + n) % JOIST_MAX_CSPACES;
            if (sh_cs[i].live) {
                return i;
            }
        }
    }
    return pick(JOIST_MAX_CSPACES + 1u);
}

/* A slot holding a capability of the wanted type (any type for NONE) 3 times
 * in 4 when there is one; otherwise any slot, one past the end included. */
static uint32_t pick_slot(uint32_t cs, uint8_t type)
{
    if (cs < JOIST_MAX_CSPACES && pick(4) != 0) {
        uint32_t start = pick(JOIST_CSPACE_SLOTS);
        for (uint32_t n = 0; n < JOIST_CSPACE_SLOTS; n++) {
            uint32_t i = (start + n) % JOIST_CSPACE_SLOTS;
            const struct sh_cap *c = &sh_cs[cs].slot[i];
            if (c->present && (type == JOIST_OBJ_NONE || c->type == type)) {
                return i;
            }
        }
    }
    return pick(JOIST_CSPACE_SLOTS + 1u);
}

/* A free slot 3 times in 4 when there is one. */
static uint32_t pick_dst(uint32_t cs)
{
    if (cs < JOIST_MAX_CSPACES && pick(4) != 0) {
        uint32_t start = pick(JOIST_CSPACE_SLOTS);
        for (uint32_t n = 0; n < JOIST_CSPACE_SLOTS; n++) {
            uint32_t i = (start + n) % JOIST_CSPACE_SLOTS;
            if (!sh_cs[cs].slot[i].present) {
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
    if (k.objects_live != sh_live || k.budget != sh_budget) {
        (void)snprintf(why, sizeof why, "objects_live %u/%u budget %llu/%llu (model/shadow)",
                       k.objects_live, sh_live, (unsigned long long)k.budget,
                       (unsigned long long)sh_budget);
        return 0;
    }
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        const struct joist_cspace_state *r = &k.cs[c];
        const struct sh_space *s = &sh_cs[c];
        if ((r->live != 0) != (s->live != 0) || r->quota != s->quota || r->charged != s->charged ||
            r->parent != s->parent || r->task_obj != s->task) {
            (void)snprintf(why, sizeof why,
                           "space %u: live %u/%d quota %u/%u charged %u/%u parent %u/%u "
                           "task %u/%u (model/shadow)",
                           c, r->live, s->live, r->quota, s->quota, r->charged, s->charged,
                           r->parent, s->parent, r->task_obj, s->task);
            return 0;
        }
        for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
            const struct joist_cap *rc = &r->slot[i];
            const struct sh_cap *sc = &s->slot[i];
            if ((rc->obj != 0) != (sc->present != 0) ||
                (rc->obj != 0 &&
                 (rc->obj - 1u != sc->obj || rc->type != sc->type || rc->rights != sc->rights))) {
                (void)snprintf(why, sizeof why, "space %u slot %u differs", c, i);
                return 0;
            }
        }
    }
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *r = &k.obj[o];
        const struct sh_obj *s = &sh_ob[o];
        int rlive = r->type != JOIST_OBJ_NONE;
        if (rlive != s->live) {
            (void)snprintf(why, sizeof why, "object %u: live %d/%d", o, rlive, s->live);
            return 0;
        }
        if (rlive && (r->type != s->type || r->donor != s->donor || r->refs != sh_refs(o) ||
                      ((r->type == JOIST_OBJ_TASK || r->type == JOIST_OBJ_THREAD) &&
                       ((r->dead != 0) != (s->dead != 0) || r->target != s->target)))) {
            (void)snprintf(why, sizeof why,
                           "object %u: type %u/%u donor %u/%u refs %u/%u dead %u/%d "
                           "target %u/%u (model/shadow)",
                           o, r->type, s->type, r->donor, s->donor, r->refs, sh_refs(o), r->dead,
                           s->dead, r->target, s->target);
            return 0;
        }
    }
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        const struct joist_thread_rec *r = &k.th[t];
        const struct sh_thread *s = &sh_th[t];
        int rlive = r->state != JOIST_T_FREE;
        if (rlive != s->live || (rlive && (r->cs != s->cs || r->obj != s->obj))) {
            (void)snprintf(why, sizeof why, "thread %u: live %d/%d cs %u/%u obj %u/%u", t, rlive,
                           s->live, r->cs, s->cs, r->obj, s->obj);
            return 0;
        }
    }
    return joist_check_invariants(&k, why, sizeof why) == 0;
}

/* ---- coverage ---- */

enum {
    OP_TASK_CREATE,
    OP_TASK_DESTROY,
    OP_SPAWN,
    OP_KILL,
    OP_SPACE_DESTROY,
    OP_OBJ_CREATE,
    OP_DERIVE,
    OP_GRANT,
    OP_DELETE,
    OP_REAP,
    OP_DESCENDANT,
    OP_COUNT
};
static const char *const op_name[OP_COUNT] = {"task_create",   "task_destroy", "spawn",     "kill",
                                              "space_destroy", "obj_create",   "derive",    "grant",
                                              "delete",        "reap",         "descendant"};
#define N_ERR 14u
static unsigned long coverage[OP_COUNT][N_ERR];
static unsigned long cascades; /* operations that destroyed more than one space */

/* ---- the run ---- */

static unsigned long env_ulong(const char *name, unsigned long def)
{
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? strtoul(v, NULL, 0) : def;
}

static uint32_t spaces_live(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < JOIST_MAX_CSPACES; i++) {
        n += (uint32_t)(sh_cs[i].live != 0);
    }
    return n;
}

static int run_seed(uint64_t seed, unsigned long steps)
{
    rng_state = seed * 0x9E3779B97F4A7C15ULL + 1u;
    joist_init(&k);
    memset(sh_cs, 0, sizeof sh_cs);
    memset(sh_ob, 0, sizeof sh_ob);
    memset(sh_th, 0, sizeof sh_th);
    sh_live = 0;

    uint32_t budget = 20u + pick(200);
    joist_cspace root;
    if (joist_boot(&k, budget, &root) != JOIST_OK) {
        fprintf(stderr, "  seed %" PRIu64 ": boot failed\n", seed);
        return 0;
    }
    sh_root = 0;
    sh_budget = budget;
    sh_cs[0].live = 1;
    sh_cs[0].quota = budget;

    for (unsigned long step = 0; step < steps; step++) {
        uint32_t roll = pick(110);
        uint32_t cs = pick_space();
        uint32_t before = spaces_live();
        int op;
        joist_err got;
        joist_err want;

        if (roll < 14) {
            op = OP_TASK_CREATE;
            uint32_t dst = pick_dst(cs);
            uint32_t donation = pick(16) == 0 ? UINT32_MAX - pick(2) : pick(12);
            want = sh_task_create_rc(cs, donation, dst);
            got = joist_task_create(&k, cs, donation, dst);
            if (got == JOIST_OK && want == JOIST_OK) {
                uint32_t ns = sh_free_space();
                uint32_t idx = k.cs[cs].slot[dst].obj - 1u;
                if (!sh_adopt(idx, JOIST_OBJ_TASK, cs, dst, ns + 1u,
                              (joist_rights)(JOIST_R_CONTROL | JOIST_R_GRANT))) {
                    (void)snprintf(why, sizeof why, "task_create reused live object %u", idx);
                    got = JOIST_E_RANGE;
                } else {
                    sh_cs[cs].quota -= donation;
                    sh_cs[ns] = (struct sh_space){
                        .live = 1, .quota = donation, .parent = cs + 1u, .task = idx + 1u};
                }
            }
        } else if (roll < 22) {
            op = OP_TASK_DESTROY;
            uint32_t slot = pick_slot(cs, JOIST_OBJ_TASK);
            want = sh_task_destroy(cs, slot);
            got = joist_task_destroy(&k, cs, slot);
        } else if (roll < 32) {
            op = OP_SPAWN;
            uint32_t ts = pick_slot(cs, JOIST_OBJ_TASK);
            uint32_t dst = pick_dst(cs);
            joist_thread out = 0;
            want = sh_spawn_rc(cs, ts, dst);
            got = joist_thread_spawn(&k, cs, ts, dst, &out);
            if (got == JOIST_OK && want == JOIST_OK) {
                uint32_t t = sh_free_thread();
                uint32_t idx = k.cs[cs].slot[dst].obj - 1u;
                if (out != t || !sh_adopt(idx, JOIST_OBJ_THREAD, cs, dst, t + 1u,
                                          (joist_rights)(JOIST_R_CONTROL | JOIST_R_GRANT))) {
                    (void)snprintf(why, sizeof why, "spawn: thread %u/%u object %u", out, t, idx);
                    got = JOIST_E_RANGE;
                } else {
                    sh_th[t] = (struct sh_thread){.live = 1,
                                                  .cs = sh_ob[sh_cs[cs].slot[ts].obj].target - 1u,
                                                  .obj = idx + 1u};
                }
            }
        } else if (roll < 38) {
            op = OP_KILL;
            uint32_t slot = pick_slot(cs, JOIST_OBJ_THREAD);
            want = sh_thread_kill(cs, slot);
            got = joist_thread_kill(&k, cs, slot);
        } else if (roll < 41) {
            op = OP_SPACE_DESTROY;
            want = sh_space_destroy(cs);
            got = joist_cspace_destroy(&k, cs);
        } else if (roll < 56) {
            op = OP_OBJ_CREATE;
            uint32_t dst = pick_dst(cs);
            /* Tasks and threads are refused here; asked for 1 time in 8. */
            uint8_t type = pick(8) == 0 ? (uint8_t)(JOIST_OBJ_TASK + pick(2))
                                        : (uint8_t)(JOIST_OBJ_ENDPOINT + pick(2));
            want = sh_obj_create_rc(cs, type, dst);
            got = joist_obj_create(&k, cs, (joist_obj_type)type, dst);
            if (got == JOIST_OK && want == JOIST_OK) {
                uint32_t idx = k.cs[cs].slot[dst].obj - 1u;
                if (!sh_adopt(idx, type, cs, dst, 0,
                              (joist_rights)(JOIST_R_SEND | JOIST_R_RECV | JOIST_R_GRANT))) {
                    (void)snprintf(why, sizeof why, "obj_create reused live object %u", idx);
                    got = JOIST_E_RANGE;
                }
            }
        } else if (roll < 66) {
            op = OP_DERIVE;
            uint32_t src = pick_slot(cs, JOIST_OBJ_NONE);
            uint32_t dst = pick_dst(cs);
            joist_rights mask = (joist_rights)pick(16);
            want = sh_copy(cs, src, cs, dst, mask, 0);
            got = joist_cap_derive(&k, cs, src, dst, mask);
        } else if (roll < 82) {
            op = OP_GRANT;
            uint32_t src = pick_slot(cs, JOIST_OBJ_NONE);
            uint32_t to = pick_space();
            uint32_t dst = pick_dst(to);
            /* Mostly keep everything, so granted caps can be granted again. */
            joist_rights mask = pick(4) == 0 ? (joist_rights)pick(16) : JOIST_R_ALL;
            want = sh_copy(cs, src, to, dst, mask, 1);
            got = joist__cap_grant(&k, cs, src, to, dst, mask);
        } else if (roll < 100) {
            op = OP_DELETE;
            uint32_t slot = pick_slot(cs, JOIST_OBJ_NONE);
            want = sh_delete(cs, slot);
            got = joist_cap_delete(&k, cs, slot);
        } else if (roll < 106) {
            op = OP_REAP;
            /* From the root half the time, so ancestry usually holds. */
            uint32_t from = pick(2) == 0 ? 0 : cs;
            uint32_t target = pick_space();
            want = sh_reap(from, target);
            got = joist_task_reap(&k, from, target);
        } else {
            op = OP_DESCENDANT;
            uint32_t start = pick(JOIST_MAX_CSPACES + 2u);
            joist_cspace mo = 0;
            uint32_t so = 0;
            want = sh_descendant(cs, start, &so);
            got = joist_task_descendant(&k, cs, start, &mo);
            if (got == JOIST_OK && want == JOIST_OK && mo != so) {
                (void)snprintf(why, sizeof why, "descendant: %u, shadow %u", mo, so);
                got = JOIST_E_STATE;
            }
        }

        coverage[op][(unsigned)got < N_ERR ? (unsigned)got : N_ERR - 1u]++;
        if (before > spaces_live() + 1u) {
            cascades++;
        }
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

JT_TEST(props_task_model_matches_shadow)
{
    unsigned long steps = env_ulong("JOIST_PROP_STEPS", 2000);
    const char *one = getenv("JOIST_PROP_SEED");
    memset(coverage, 0, sizeof coverage);
    cascades = 0;

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
    fprintf(stderr, "  coverage cascades     %lu operations destroyed more than one task\n",
            cascades);
}
