/* SPDX-License-Identifier: GPL-3.0-only */
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
                                        [JX_SC_CREATE] = "sc_create",
                                        [JX_SC_BIND] = "sc_bind",
                                        [JX_SC_UNBIND] = "sc_unbind",
                                        [JX_SET_MCP] = "set_mcp",
                                        [JX_REGS] = "regs",
                                        [JX_ABORT] = "abort",
                                        [JX_SUSPEND] = "suspend",
                                        [JX_RESUME] = "resume",
                                        [JX_SET_FAULT] = "set_fault",
                                        [JX_FAULT] = "fault",
                                        [JX_SPLIT] = "split",
                                        [JX_MAP] = "map",
                                        [JX_UNMAP] = "unmap",
                                        [JX_PROTECT] = "protect",
                                        [JX_DONATE] = "donate",
                                        [JX_MAP_QUERY] = "map_query",
                                        [JX_CONTINUE] = "continue",
                                        [JX_GIVE] = "give",
                                        [JX_OBJ_KILL] = "obj_kill",
                                        [JX_COPY_BATCH] = "copy_batch",
                                        [JX_TICK] = "tick",
                                        [JX_SC_FALLBACK] = "sc_bind_fallback",
                                        [JX_REGION] = "region"};

/* Relative frequency of each operation, out of their sum. */
static const uint8_t weight[JX_OPS] = {
    [JX_SPACE_CREATE] = 1, [JX_SPACE_DESTROY] = 1, [JX_OBJ_CREATE] = 10, [JX_DERIVE] = 6,
    [JX_MINT] = 6,         [JX_GRANT] = 10,        [JX_DELETE] = 8,      [JX_TASK_CREATE] = 5,
    [JX_TASK_DESTROY] = 2, [JX_SPAWN] = 5,         [JX_KILL] = 2,        [JX_TH_CREATE] = 2,
    [JX_TH_DESTROY] = 1,   [JX_CALL] = 11,         [JX_RECV] = 6,        [JX_REPLY] = 4,
    [JX_REPLY_RECV] = 4,   [JX_SIGNAL] = 5,        [JX_WAIT] = 5,        [JX_BIND] = 3,
    [JX_DEADNAME] = 3,     [JX_COLLECT] = 3,       [JX_REAP] = 2,        [JX_DESCENDANT] = 2,
    [JX_SAVE] = 3,         [JX_SAVED] = 3,         [JX_GROW] = 2,        [JX_SHRINK] = 2,
    [JX_RECV_SLOTS] = 3,   [JX_SC_CREATE] = 3,     [JX_SC_BIND] = 4,     [JX_SC_UNBIND] = 2,
    [JX_SET_MCP] = 1,      [JX_REGS] = 2,          [JX_ABORT] = 2,       [JX_SUSPEND] = 1,
    [JX_RESUME] = 2,       [JX_SET_FAULT] = 3,     [JX_FAULT] = 2,       [JX_SPLIT] = 2,
    [JX_MAP] = 5,          [JX_UNMAP] = 2,         [JX_PROTECT] = 2,     [JX_DONATE] = 2,
    [JX_MAP_QUERY] = 3,
    [JX_CONTINUE] = 4,     [JX_GIVE] = 4,
    [JX_OBJ_KILL] = 2,     [JX_COPY_BATCH] = 4,    [JX_TICK] = 6,        [JX_SC_FALLBACK] = 3,
    [JX_REGION] = 6};

static uint64_t boot_budget;

int jx_op_cascades(unsigned op)
{
    return op == JX_SPACE_DESTROY || op == JX_DELETE || op == JX_TASK_DESTROY || op == JX_KILL ||
           op == JX_TH_DESTROY || op == JX_REAP ||
           op == JX_FAULT || /* a fault with no handler destroys its task (0009) */
           op == JX_CONTINUE ||
           op == JX_OBJ_KILL; /* a killed frame's sweep (0020) */
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

/* ---- the kernel hooks, recorded (Phase 3 step 4, Phase 4 step 2) ----
 * The executor installs hooks that keep a shadow of what the kernel would
 * have built: which spaces have page tables, which tables each has, and what
 * each page maps. After every step the shadow must equal the core's own
 * record, so a missing, extra or wrong hook call anywhere in the core is
 * caught. Pages lent for tables (0021) live in a simulated memory. */
static uint64_t jx_mem[JX_MEM_PAGES][JOIST_PAGE_SIZE / 8u];

static void *hk_page(void *ctx, uint64_t pfn)
{
    (void)ctx;
    return pfn >= JX_MEM_BASE && pfn < JX_MEM_BASE + JX_MEM_PAGES ? jx_mem[pfn - JX_MEM_BASE] : NULL;
}

#define HS_TABLES 256u
#define HS_PAGES 2048u
static struct {
    uint8_t live[JOIST_MAX_CSPACES];
    struct {
        uint32_t space1; /* space plus one; 0 = free */
        unsigned level;
        uint64_t region; /* va >> (9 * (level + 1)) */
    } t[HS_TABLES];
    struct {
        uint32_t space1;
        uint64_t va, pa;
        unsigned perms;
    } p[HS_PAGES];
    int bad;
    char why[160];
} hshadow;

static void hs_fail(const char *what, uint32_t space)
{
    if (!hshadow.bad) {
        (void)snprintf(hshadow.why, sizeof hshadow.why, "hooks: %s (space %u)", what, space);
    }
    hshadow.bad = 1;
}

static int hs_table(uint32_t space, unsigned level, uint64_t va)
{
    uint64_t r = va >> (9u * (level + 1u));
    for (uint32_t i = 0; i < HS_TABLES; i++) {
        if (hshadow.t[i].space1 == space + 1u && hshadow.t[i].level == level && hshadow.t[i].region == r) {
            return (int)i;
        }
    }
    return -1;
}

static int hs_page(uint32_t space, uint64_t va)
{
    for (uint32_t i = 0; i < HS_PAGES; i++) {
        if (hshadow.p[i].space1 == space + 1u && hshadow.p[i].va == va) {
            return (int)i;
        }
    }
    return -1;
}

static void hk_space_new(void *ctx, uint32_t space)
{
    (void)ctx;
    if (space >= JOIST_MAX_CSPACES || hshadow.live[space]) {
        hs_fail("space_new for a space that has tables", space);
        return;
    }
    hshadow.live[space] = 1;
}

static void hk_space_gone(void *ctx, uint32_t space)
{
    (void)ctx;
    if (space >= JOIST_MAX_CSPACES || !hshadow.live[space]) {
        hs_fail("space_gone for a space without tables", space);
        return;
    }
    /* Its tables and pages go with it, all at once. */
    for (uint32_t i = 0; i < HS_TABLES; i++) {
        hshadow.t[i].space1 = hshadow.t[i].space1 == space + 1u ? 0 : hshadow.t[i].space1;
    }
    for (uint32_t i = 0; i < HS_PAGES; i++) {
        hshadow.p[i].space1 = hshadow.p[i].space1 == space + 1u ? 0 : hshadow.p[i].space1;
    }
    hshadow.live[space] = 0;
}

static void hk_table_new(void *ctx, uint32_t space, uint64_t va, unsigned level, uint64_t pfn)
{
    (void)ctx;
    if (space >= JOIST_MAX_CSPACES || !hshadow.live[space] || level >= JOIST_PT_LEVELS ||
        hk_page(NULL, pfn) == NULL) {
        hs_fail("table_new outside a live space or the lent pages", space);
        return;
    }
    if (hs_table(space, level, va) >= 0 || (level + 1u < JOIST_PT_LEVELS && hs_table(space, level + 1u, va) < 0)) {
        hs_fail("table_new where one is, or under none", space);
        return;
    }
    for (uint32_t i = 0; i < HS_TABLES; i++) {
        if (hshadow.t[i].space1 == 0) {
            hshadow.t[i].space1 = space + 1u;
            hshadow.t[i].level = level;
            hshadow.t[i].region = va >> (9u * (level + 1u));
            return;
        }
    }
    hs_fail("more tables than the shadow holds", space);
}

static void hk_table_gone(void *ctx, uint32_t space, uint64_t va, unsigned level)
{
    (void)ctx;
    int t = space < JOIST_MAX_CSPACES ? hs_table(space, level, va) : -1;
    if (t < 0) {
        hs_fail("table_gone for no table", space);
        return;
    }
    uint64_t r = hshadow.t[t].region;
    for (uint32_t i = 0; i < HS_TABLES; i++) {
        if (hshadow.t[i].space1 == space + 1u && hshadow.t[i].level + 1u == level &&
            hshadow.t[i].region >> JOIST_PT_SHIFT(1) == r) {
            hs_fail("table_gone over a table", space);
        }
    }
    for (uint32_t i = 0; level == 0 && i < HS_PAGES; i++) {
        if (hshadow.p[i].space1 == space + 1u && hshadow.p[i].va >> JOIST_PT_SHIFT(1) == r) {
            hs_fail("table_gone over a mapped page", space);
        }
    }
    hshadow.t[t].space1 = 0;
}

static void hk_map(void *ctx, uint32_t space, uint64_t va, uint64_t pa, uint64_t npages,
                   unsigned perms, int replace)
{
    (void)ctx;
    if (space >= JOIST_MAX_CSPACES || !hshadow.live[space] || npages == 0 ||
        hs_table(space, 0, va) < 0 || (va + npages - 1u) >> JOIST_PT_SHIFT(1) != va >> JOIST_PT_SHIFT(1)) {
        hs_fail("map without a table, or across tables", space);
        return;
    }
    for (uint64_t n = 0; n < npages; n++) {
        int at = hs_page(space, va + n);
        if ((at >= 0) != (replace != 0)) {
            hs_fail(replace ? "replace of a page not mapped" : "map over a mapped page", space);
            return;
        }
        for (uint32_t i = 0; at < 0 && i < HS_PAGES; i++) {
            if (hshadow.p[i].space1 == 0) {
                at = (int)i;
            }
        }
        if (at < 0) {
            hs_fail("more pages than the shadow holds", space);
            return;
        }
        hshadow.p[at].space1 = space + 1u;
        hshadow.p[at].va = va + n;
        hshadow.p[at].pa = pa + n;
        hshadow.p[at].perms = perms;
    }
}

/* map_query's answer for [va, va + n) of the task in slot ts of space ms,
 * against the hooks' record of what is mapped: present, its permissions, a
 * writable page's ceiling, and the page offset exactly for the frame in
 * slot fs (JOIST_SLOT_NONE: none). */
static void check_query(const struct joist_kernel *k, uint32_t ms, uint32_t ts, uint64_t va,
                        uint64_t n, uint32_t fs, const uint64_t *q)
{
    const struct joist_cap *tc = joist__slot_c(k, ms, ts);
    uint32_t space = k->obj[tc->obj - 1u].target - 1u;
    const struct joist_object *f =
        fs == JOIST_SLOT_NONE ? NULL : &k->obj[joist__slot_c(k, ms, fs)->obj - 1u];
    for (uint64_t i = 0; i < n; i++) {
        int at = hs_page(space, va + i);
        if ((q[i] != 0) != (at >= 0) || (q[i] != 0 && (q[i] & JOIST_QUERY_PRESENT) == 0)) {
            hs_fail("map_query: present where the hooks mapped nothing, or the reverse", space);
            return;
        }
        if (at < 0) {
            continue;
        }
        uint64_t pa = hshadow.p[at].pa;
        int in = f != NULL && pa >= f->base && pa - f->base < f->npages;
        if ((q[i] & (JOIST_MAP_W | JOIST_MAP_X)) != hshadow.p[at].perms ||
            ((q[i] & JOIST_MAP_W) != 0 && (q[i] & JOIST_QUERY_CEIL_W) == 0) ||
            ((q[i] & JOIST_QUERY_FRAME) != 0) != in || (in && JOIST_QUERY_OFF(q[i]) != pa - f->base) ||
            (!in && JOIST_QUERY_OFF(q[i]) != 0)) {
            hs_fail("map_query: permissions, ceiling or frame unlike the hooks' record", space);
            return;
        }
    }
}

static void hk_unmap(void *ctx, uint32_t space, uint64_t va, uint64_t npages)
{
    (void)ctx;
    for (uint64_t n = 0; n < npages; n++) {
        int at = hs_page(space, va + n);
        if (at < 0) {
            hs_fail("unmap of a page not mapped", space);
            return;
        }
        hshadow.p[at].space1 = 0;
    }
}

static void hk_protect(void *ctx, uint32_t space, uint64_t va, uint64_t npages, unsigned perms)
{
    (void)ctx;
    for (uint64_t n = 0; n < npages; n++) {
        int at = hs_page(space, va + n);
        if (at < 0) {
            hs_fail("protect of a page not mapped", space);
            return;
        }
        hshadow.p[at].perms = perms;
    }
}

/* The shadow and the core agree: the same spaces have tables, the same
 * tables exist, and every page maps the same physical page with the same
 * permissions. 1 if so. */
static int hooks_agree(const struct joist_kernel *k, char *why, unsigned whylen)
{
    if (hshadow.bad) {
        (void)snprintf(why, whylen, "%s", hshadow.why);
        return 0;
    }
    uint32_t tables = 0;
    uint32_t pages = 0;
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        if (hshadow.live[s] != k->cs[s].live) {
            (void)snprintf(why, whylen, "hooks: space %u live %u, but tables %u", s, k->cs[s].live,
                           hshadow.live[s]);
            return 0;
        }
        if (!k->cs[s].live || k->cs[s].dying) {
            continue; /* a dying space's tables go with space_gone */
        }
        for (uint64_t t2 = 0; t2 < JOIST_PT_TOP; t2++) {
            uint64_t e2 = k->cs[s].pt_top[t2];
            if (e2 == 0) {
                continue;
            }
            tables++;
            if (hs_table(s, 2, t2 << JOIST_PT_SHIFT(3)) < 0) {
                (void)snprintf(why, whylen, "hooks: space %u table 2/%llu never made", s, (unsigned long long)t2);
                return 0;
            }
            const uint64_t *n2 = hk_page(NULL, JOIST_PT_SHADOW(e2));
            for (uint64_t t1 = 0; n2 != NULL && t1 < JOIST_PT_ENTRIES; t1++) {
                if (n2[t1] == 0) {
                    continue;
                }
                tables++;
                uint64_t va1 = (t2 << JOIST_PT_SHIFT(3)) | (t1 << JOIST_PT_SHIFT(2));
                if (hs_table(s, 1, va1) < 0) {
                    (void)snprintf(why, whylen, "hooks: space %u table 1 at %#llx never made", s,
                                   (unsigned long long)va1);
                    return 0;
                }
                const uint64_t *n1 = hk_page(NULL, JOIST_PT_SHADOW(n2[t1]));
                for (uint64_t t0 = 0; n1 != NULL && t0 < JOIST_PT_ENTRIES; t0++) {
                    if (n1[t0] == 0) {
                        continue;
                    }
                    tables++;
                    uint64_t va0 = va1 | (t0 << JOIST_PT_SHIFT(1));
                    if (hs_table(s, 0, va0) < 0) {
                        (void)snprintf(why, whylen, "hooks: space %u table 0 at %#llx never made", s,
                                       (unsigned long long)va0);
                        return 0;
                    }
                    const uint64_t *lf = hk_page(NULL, JOIST_PT_SHADOW(n1[t0]));
                    for (uint64_t i = 0; lf != NULL && i < JOIST_PT_ENTRIES; i++) {
                        uint64_t pe = lf[i];
                        if (pe == 0) {
                            continue;
                        }
                        pages++;
                        int at = hs_page(s, va0 | i);
                        const struct joist_object *f = &k->obj[JOIST_PTE_FRAME(pe)];
                        if (at < 0 || hshadow.p[at].pa != f->base + JOIST_PTE_OFF(pe) ||
                            hshadow.p[at].perms != JOIST_PTE_PERMS(pe)) {
                            (void)snprintf(why, whylen, "hooks: space %u page %#llx not carried out as recorded",
                                           s, (unsigned long long)(va0 | i));
                            return 0;
                        }
                    }
                }
            }
        }
    }
    uint32_t hs_tables = 0;
    uint32_t hs_pages = 0;
    for (uint32_t i = 0; i < HS_TABLES; i++) {
        uint32_t s1 = hshadow.t[i].space1;
        hs_tables += (uint32_t)(s1 != 0 && !k->cs[s1 - 1u].dying);
    }
    for (uint32_t i = 0; i < HS_PAGES; i++) {
        uint32_t s1 = hshadow.p[i].space1;
        hs_pages += (uint32_t)(s1 != 0 && !k->cs[s1 - 1u].dying);
    }
    if (hs_tables != tables || hs_pages != pages) {
        (void)snprintf(why, whylen, "hooks: %u tables and %u pages carried out, %u and %u in the core",
                       hs_tables, hs_pages, tables, pages);
        return 0;
    }
    return 1;
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
    memset(&hshadow, 0, sizeof hshadow);
    memset(jx_mem, 0xa5, sizeof jx_mem); /* lent pages must be zeroed before use */
    k->hooks = (struct joist_hooks){.space_new = hk_space_new,
                                    .space_gone = hk_space_gone,
                                    .table_new = hk_table_new,
                                    .table_gone = hk_table_gone,
                                    .map = hk_map,
                                    .unmap = hk_unmap,
                                    .protect = hk_protect,
                                    .page = hk_page};
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
    /* The first thread is an active client with a context of its own; the
     * other two are passive, so a call can be lent on (a donation chain)
     * even on the one endpoint. */
    if (joist_sc_create(k, root, 6, 10, 1, 2) == JOIST_OK) {
        (void)joist__sc_bind_thread(k, k->cs[root].slot[6].obj - 1u, 0);
    }
    /* The root task's fault handler is its endpoint (slot 0), so a fault
     * usually reaches a thread that receives there. (The root has no task
     * capability for joist_task_set_fault: set directly, as setup.) */
    k->cs[root].fault_slot = 1;
    /* Memory, as the kernel hands it to the root at boot (0013): two frames
     * to map, one in the simulated memory to lend for page tables (0021),
     * and the root's capability to itself, to map into its own space. */
    (void)joist_boot_frame(k, root, 7, 0x100, 32);
    (void)joist_boot_frame(k, root, 8, 0x1000, 8);
    (void)joist_boot_frame(k, root, 9, JX_MEM_BASE, JX_MEM_PAGES);
    (void)joist_boot_self(k, root, 10);
    /* Four frames of it, one lent to the root's own space at once. */
    (void)joist_frame_split(k, root, 9, JX_MEM_PAGES / 4u, 11);
    (void)joist_frame_split(k, root, 11, JX_MEM_PAGES / 4u, 12);
    (void)joist_frame_split(k, root, 12, JX_MEM_PAGES / 4u, 13);
    (void)joist_space_donate(k, root, 10, 9);
}

/* What a step did, for the checks that need more than the final state. */
struct last {
    uint32_t t;       /* the acting thread */
    uint32_t obj;     /* SIGNAL: the notification object */
    joist_badge bits; /* SIGNAL: the badge signalled */
    uint64_t word;    /* SIGNAL: the notification's pending word before */
    uint32_t caller;  /* REPLY: the caller the reply capability named, plus one */
    int faulting;     /* REPLY: that caller was blocked in a fault call */
    uint64_t r0;      /* REPLY: the reply's first register (for a fault: the verdict) */
};

/* Before a reply to `caller` (plus one): note whether it is a fault call,
 * and if so, resume it half the time (r[0] = JOIST_FAULT_RESUME) and
 * terminate it otherwise (0009). */
static void reply_prep(const struct joist_kernel *k, struct jx_source *src, struct last *ls,
                       struct joist_msg *m)
{
    ls->faulting = ls->caller != 0 && ls->caller <= JOIST_MAX_THREADS &&
                   k->th[ls->caller - 1u].faulting;
    if (ls->faulting && ch(src, 2) == 0) {
        m->r[0] = JOIST_FAULT_RESUME;
    }
    ls->r0 = m->r[0];
}

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
        uint32_t d = pick_dst(k, src, cs);
        joist_err e = joist_thread_spawn(k, cs, ts, d, &out);
        /* Half the new threads get a context of their own at once, through
         * the public calls, so there are active clients to lend time to the
         * passive rest (a random space rarely holds both capabilities). */
        if (e == JOIST_OK && ch(src, 2) == 0) {
            uint32_t sd = pick_dst(k, src, cs);
            if (joist_sc_create(k, cs, sd, (uint8_t)ch(src, 256), 1, 2) == JOIST_OK) {
                (void)joist_sc_bind(k, cs, sd, d);
            }
        }
        return e;
    }
    case JX_KILL:
        return joist_thread_kill(k, cs, pick_slot(k, src, cs, JOIST_OBJ_THREAD));
    case JX_TH_CREATE: {
        joist_thread out;
        joist_err e = joist_thread_create(k, cs, &out);
        /* Half the fixture threads are active, as with spawn. */
        if (e == JOIST_OK && ch(src, 2) == 0) {
            uint32_t sd = pick_dst(k, src, cs);
            if (joist_sc_create(k, cs, sd, (uint8_t)ch(src, 256), 1, 2) == JOIST_OK) {
                (void)joist__sc_bind_thread(k, joist__slot_c(k, cs, sd)->obj - 1u, out);
            }
        }
        return e;
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
        reply_prep(k, src, ls, &m);
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
        reply_prep(k, src, ls, &m);
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
    case JX_SC_CREATE: {
        /* Valid parameters 7 times in 8; a priority anywhere, so the mcp
         * check fires once mcps have been lowered. */
        uint64_t period = 1u + ch(src, 100);
        uint64_t budget = ch(src, 8) == 0 ? ch(src, 200) : 1u + ch(src, (uint32_t)period);
        return joist_sc_create(k, cs, pick_dst(k, src, cs), (uint8_t)ch(src, 256), budget,
                               period);
    }
    case JX_SC_BIND:
        return joist_sc_bind(k, cs, pick_slot(k, src, cs, JOIST_OBJ_SCHED),
                             pick_slot(k, src, cs, JOIST_OBJ_THREAD));
    case JX_SC_UNBIND:
        return joist_sc_unbind(k, cs, pick_slot(k, src, cs, JOIST_OBJ_SCHED));
    case JX_SC_FALLBACK:
        return joist_sc_bind_fallback(k, cs, pick_slot(k, src, cs, JOIST_OBJ_SCHED),
                                      pick_slot(k, src, cs, JOIST_OBJ_THREAD));
    case JX_SET_MCP:
        return joist_task_set_mcp(k, cs, pick_slot(k, src, cs, JOIST_OBJ_TASK),
                                  (uint8_t)ch(src, 256));
    case JX_REGS: {
        /* Write a register record and read it back through the same
         * capability: what was written is what is read (0010). */
        uint32_t s = pick_slot(k, src, cs, JOIST_OBJ_THREAD);
        struct joist_regs r = {0};
        r.pc = step;
        r.r[ch(src, JOIST_REGS)] = step;
        joist_err e = joist_thread_write_regs(k, cs, s, &r);
        struct joist_regs back;
        if (e == JOIST_OK && (joist_thread_read_regs(k, cs, s, &back) != JOIST_OK ||
                              memcmp(&back, &r, sizeof r) != 0)) {
            (void)snprintf(why, whylen, "0010: registers read back differ from those written");
            *bad = 1;
        }
        return e;
    }
    case JX_ABORT:
        return joist_thread_abort(k, cs, pick_slot(k, src, cs, JOIST_OBJ_THREAD));
    case JX_SUSPEND:
        return joist_thread_suspend(k, cs, pick_slot(k, src, cs, JOIST_OBJ_THREAD));
    case JX_RESUME:
        return joist_thread_resume(k, cs, pick_slot(k, src, cs, JOIST_OBJ_THREAD));
    case JX_SET_FAULT: {
        /* A handler slot that usually holds an endpoint, sometimes not. */
        joist_slot hs = ch(src, 8) == 0 ? JOIST_SLOT_NONE : pick_slot(k, src, cs, JOIST_OBJ_ENDPOINT);
        /* No thread capability on page faults half the time; an unknown
         * flag now and then. */
        unsigned flags = ch(src, 16) == 0 ? ch(src, 4) : ch(src, 2) * JOIST_FAULT_NO_THREAD_CAP;
        return ch(src, 3) == 0
                   ? joist_task_set_fault(k, cs, pick_slot(k, src, cs, JOIST_OBJ_TASK), hs, flags)
                   : joist_thread_set_fault(k, cs, pick_slot(k, src, cs, JOIST_OBJ_THREAD), hs, flags);
    }
    case JX_SPLIT:
    case JX_MAP:
    case JX_UNMAP:
    case JX_PROTECT:
    case JX_DONATE:
    case JX_MAP_QUERY: {
        /* From the root half the time: it holds the boot frames, the task
         * capabilities to map into, and its own (slot 10). */
        uint32_t ms = ch(src, 2) == 0 && k->root != 0 ? k->root - 1u : cs;
        if (op == JX_SPLIT) {
            uint32_t f = pick_slot(k, src, ms, JOIST_OBJ_FRAME);
            return joist_frame_split(k, ms, f, ch(src, 34), pick_dst(k, src, ms));
        }
        uint32_t ts = pick_slot(k, src, ms, JOIST_OBJ_TASK);
        if (op == JX_DONATE) {
            return joist_space_donate(k, ms, ts, pick_slot(k, src, ms, JOIST_OBJ_FRAME));
        }
        /* A few virtual places, in two last-level tables and two top-level
         * entries, so runs collide; now and then the top of the user half. */
        uint64_t va = ch(src, 16) == 0 ? JOIST_USER_PAGES - 1u - ch(src, 40)
                                       : (ch(src, 4) == 0 ? (uint64_t)1 << JOIST_PT_SHIFT(3) : 0x400u) +
                                             (ch(src, 3) == 0 ? 512u : 0u) + ch(src, 24);
        uint64_t n = ch(src, 8) == 0 ? ch(src, 600) : 1u + ch(src, 6);
        unsigned perms = ch(src, 8) == 0 ? ch(src, 8) : ch(src, 3);
        if (op == JX_UNMAP) {
            return joist_unmap_range(k, ms, ts, va, n);
        }
        if (op == JX_PROTECT) {
            return joist_protect(k, ms, ts, va, n, perms);
        }
        if (op == JX_MAP_QUERY) {
            /* Usually naming a frame, now and then none; sometimes too many pages. */
            uint32_t fs = ch(src, 4) == 0 ? JOIST_SLOT_NONE : pick_slot(k, src, ms, JOIST_OBJ_FRAME);
            uint64_t qn = ch(src, 8) == 0 ? ch(src, 80) : 1u + ch(src, 12);
            static uint64_t q[JOIST_QUERY_MAX];
            joist_err e = joist_map_query(k, ms, ts, va, qn, fs, q);
            if (e == JOIST_OK) {
                check_query(k, ms, ts, va, qn, fs, q);
            }
            return e;
        }
        uint32_t f = pick_slot(k, src, ms, JOIST_OBJ_FRAME);
        uint64_t off = ch(src, 4) == 0 ? ch(src, 40) : ch(src, 4);
        if (ch(src, 4) == 0) {
            /* A replace, usually of what one page there holds. */
            uint32_t of = pick_slot(k, src, ms, JOIST_OBJ_FRAME);
            uint64_t ooff = ch(src, 4);
            const struct joist_cap *tc = ms < JOIST_MAX_CSPACES ? joist__slot_c(k, ms, ts) : NULL;
            if (tc != NULL && tc->obj != 0 && tc->type == JOIST_OBJ_TASK && ch(src, 4) != 0) {
                uint32_t sp = k->obj[tc->obj - 1u].target;
                uint64_t *pe = NULL;
                if (sp != 0 && sp <= JOIST_MAX_CSPACES && k->cs[sp - 1u].pt_top[va >> JOIST_PT_SHIFT(3)] != 0) {
                    const uint64_t *n2 = hk_page(NULL, JOIST_PT_SHADOW(k->cs[sp - 1u].pt_top[va >> JOIST_PT_SHIFT(3)]));
                    uint64_t e1 = n2 != NULL ? n2[(va >> JOIST_PT_SHIFT(2)) & (JOIST_PT_ENTRIES - 1u)] : 0;
                    const uint64_t *n1 = e1 != 0 ? hk_page(NULL, JOIST_PT_SHADOW(e1)) : NULL;
                    uint64_t e0 = n1 != NULL ? n1[(va >> JOIST_PT_SHIFT(1)) & (JOIST_PT_ENTRIES - 1u)] : 0;
                    pe = e0 != 0 ? hk_page(NULL, JOIST_PT_SHADOW(e0)) : NULL;
                }
                if (pe != NULL && pe[va & (JOIST_PT_ENTRIES - 1u)] != 0) {
                    uint64_t e0 = pe[va & (JOIST_PT_ENTRIES - 1u)];
                    ooff = JOIST_PTE_OFF(e0);
                    /* Usually name the frame that is mapped there, so that
                     * replaces succeed (and the kernel's side of one is
                     * checked: the hooks' shadow). */
                    for (uint32_t sl = 0; ch(src, 4) != 0 && sl < JOIST_CSPACE_SLOTS; sl++) {
                        const struct joist_cap *oc = joist__slot_c(k, ms, sl);
                        if (oc != NULL && oc->type == JOIST_OBJ_FRAME && oc->obj == JOIST_PTE_FRAME(e0) + 1u &&
                            (oc->rights & JOIST_R_CONTROL) != 0) {
                            of = sl;
                            break;
                        }
                    }
                }
            }
            return joist_map_replace(k, ms, f, off, n, ts, va, perms, of, ooff);
        }
        return joist_map(k, ms, f, off, n, ts, va, perms);
    }
    case JX_REGION: {
        /* From the root half the time, as the mapping calls. Usually in
         * tables of their own (0x800, 0xa00), and one time in four in those
         * JX_MAP uses (0x400, 0x600, and at the second top-level entry), so
         * regions and plain mappings meet without crowding plain ones out;
         * now and then a range off a table or past the user half. */
        uint32_t ms = ch(src, 2) == 0 && k->root != 0 ? k->root - 1u : cs;
        uint64_t base = ch(src, 4) != 0 ? 0x800u
                        : ch(src, 4) == 0 ? (uint64_t)1 << JOIST_PT_SHIFT(3)
                                          : 0x400u;
        uint64_t va = base + (ch(src, 3) == 0 ? 512u : 0u) + ch(src, 24);
        uint64_t n = ch(src, 8) == 0 ? ch(src, 600) : 1u + ch(src, 6);
        unsigned perms = ch(src, 8) == 0 ? ch(src, 8) : ch(src, 3);
        uint32_t rs = pick_slot(k, src, ms, JOIST_OBJ_REGION);
        const struct joist_cap *rc = ms < JOIST_MAX_CSPACES ? joist__slot_c(k, ms, rs) : NULL;
        int have = rc != NULL && rc->obj != 0 && rc->type == JOIST_OBJ_REGION && !k->obj[rc->obj - 1u].dead;
        /* No live region to use: usually make one; with one, its extents
         * first, so that maps have pages to map. */
        uint32_t sub = !have && ch(src, 4) != 0 ? 0u
                       : have && k->reg[k->obj[rc->obj - 1u].target - 1u].next == 0 && ch(src, 2) == 0 ? 1u
                                                                                                    : ch(src, 6);
        switch (sub) {
        case 0: {
            uint64_t lo = ch(src, 16) == 0 ? JOIST_USER_PAGES - 512u * ch(src, 3) - ch(src, 2)
                                           : base + (ch(src, 3) == 0 ? 512u : 0u) +
                                                 (ch(src, 16) == 0 ? 1u : 0u);
            uint32_t tables = ch(src, 16) == 0 ? ch(src, JOIST_REGION_TABLES + 2u) : 1u + ch(src, 2);
            unsigned ceil = ch(src, 8) == 0 ? ch(src, 8) : ch(src, 4);
            return joist_region_create(k, ms, pick_slot(k, src, ms, JOIST_OBJ_TASK), lo, tables, ceil,
                                       pick_dst(k, src, ms));
        }
        case 1: {
            uint32_t x;
            uint64_t off = ch(src, 4) == 0 ? ch(src, 40) : ch(src, 4);
            uint64_t np = ch(src, 8) == 0 ? ch(src, 40) : 1u + ch(src, 6);
            return joist_region_add(k, ms, rs, pick_slot(k, src, ms, JOIST_OBJ_FRAME), off, np, &x);
        }
        case 2:
        case 3:
            return joist_region_map(k, ms, rs, ch(src, JOIST_REGION_EXTENTS + 1u), ch(src, 8), n, va,
                                    perms);
        case 4:
            return joist_region_unmap(k, ms, rs, va, n);
        default:
            return joist_region_protect(k, ms, rs, va, n, perms);
        }
    }
    case JX_GIVE: {
        /* Into a task this space holds, a free slot there 3 times in 4. */
        uint32_t ts = pick_slot(k, src, cs, JOIST_OBJ_TASK);
        const struct joist_cap *tc = cs < JOIST_MAX_CSPACES ? joist__slot_c(k, cs, ts) : NULL;
        uint32_t target = tc != NULL && tc->obj != 0 && tc->type == JOIST_OBJ_TASK
                              ? k->obj[tc->obj - 1u].target
                              : 0;
        uint32_t d = target != 0 && target <= JOIST_MAX_CSPACES ? pick_dst(k, src, target - 1u)
                                                               : ch(src, JOIST_CSPACE_SLOTS + 1u);
        joist_rights mask = ch(src, 3) == 0 ? (joist_rights)ch(src, 16) : JOIST_R_ALL;
        return joist_cap_give(k, cs, ts, pick_slot(k, src, cs, JOIST_OBJ_NONE), d, mask);
    }
    case JX_OBJ_KILL: {
        /* Usually a frame (from the root half the time: it holds the boot
         * frames); afterwards it must be dead, and mapped nowhere once its
         * sweep is done: at once unless preempted (0020). */
        uint32_t ks = ch(src, 2) == 0 && k->root != 0 ? k->root - 1u : cs;
        uint32_t kind = ch(src, 4);
        uint32_t s = pick_slot(k, src, ks,
                               kind >= 2u ? JOIST_OBJ_FRAME : kind == 1u ? JOIST_OBJ_REGION : JOIST_OBJ_NONE);
        joist_err e = joist_obj_kill(k, ks, s);
        if (e == JOIST_OK && joist__slot_c(k, ks, s)->type == JOIST_OBJ_REGION) {
            /* A killed region: dead, and swept at once unless preempted (0030). */
            const struct joist_object *ro = &k->obj[joist__slot_c(k, ks, s)->obj - 1u];
            const struct joist_region *rr = &k->reg[ro->target - 1u];
            /* (A region of a task being destroyed waits for its task's own
             * sweep: joist__regions_space_gone.) */
            if (!ro->dead || (k->preempt == 0 && rr->dying && joist__usable(k, rr->space - 1u))) {
                (void)snprintf(why, whylen, "0030: killed region %u not dead or not swept",
                               joist__slot_c(k, ks, s)->obj - 1u);
                *bad = 1;
            }
            return e;
        }
        if (e == JOIST_OK) {
            uint32_t f = joist__slot_c(k, ks, s)->obj;
            if (k->preempt == 0 && k->obj[f - 1u].mapcount != 0) {
                (void)snprintf(why, whylen, "0022: killed frame %u still mapped", f - 1u);
                *bad = 1;
            }
            if (!k->obj[f - 1u].dead) {
                (void)snprintf(why, whylen, "0022: killed frame %u not dead", f - 1u);
                *bad = 1;
            }
        }
        return e;
    }
    case JX_COPY_BATCH: {
        /* From around an occupied slot, into a task this space holds; now
         * and then too many, an unknown flag, or a range off the end. */
        uint32_t ts = pick_slot(k, src, cs, JOIST_OBJ_TASK);
        uint32_t first = pick_slot(k, src, cs, JOIST_OBJ_NONE);
        first = first >= 4u && ch(src, 2) == 0 ? first - ch(src, 4) : first;
        uint32_t count = ch(src, 8) == 0 ? JOIST_COPY_BATCH + 1u + ch(src, 4)
                                         : ch(src, JOIST_COPY_BATCH + 1u);
        joist_rights mask = ch(src, 3) == 0 ? (joist_rights)ch(src, 16) : JOIST_R_ALL;
        uint32_t flags = ch(src, 8) == 0 ? ch(src, 4) : ch(src, 2);
        uint32_t done = 0;
        uint32_t skipped = 0;
        joist_err e = joist_cap_copy_batch(k, cs, ts, first, count, mask, flags, &done, &skipped);
        /* What was copied is the source's, masked (I8), badge kept, in the
         * same slot of the task; nothing at all on a refusal. */
        if (done != 0) { /* (so cs and the task capability were good) */
            const struct joist_cap *tc = joist__slot_c(k, cs, ts);
            uint32_t child = tc->obj != 0 && tc->type == JOIST_OBJ_TASK ? k->obj[tc->obj - 1u].target : 0;
            for (uint32_t i = 0; i < done && child != 0; i++) {
                const struct joist_cap *sc = joist__slot_c(k, cs, first + i);
                const struct joist_cap *dc = joist__slot_c(k, child - 1u, first + i);
                if (sc == NULL || sc->obj == 0 || (skipped >> i & 1u) != 0) {
                    continue;
                }
                if (dc == NULL || dc->obj != sc->obj || dc->badge != sc->badge ||
                    dc->rights != (joist_rights)(sc->rights & mask)) {
                    (void)snprintf(why, whylen, "0022: slot %u not copied as it should be",
                                   first + i);
                    *bad = 1;
                }
            }
        }
        if (e != JOIST_OK && e != JOIST_E_OCCUPIED && !(e == JOIST_E_RANGE && done != 0) &&
            (done != 0 || skipped != 0)) {
            (void)snprintf(why, whylen, "0022: a refused batch reports %u done", done);
            *bad = 1;
        }
        if (done > count || (e == JOIST_OK && done != count)) {
            (void)snprintf(why, whylen, "0022: batch of %u reports %u done", count, done);
            *bad = 1;
        }
        return e;
    }
    case JX_CONTINUE:
        /* Resume pending destruction: a few slots, or to the end. */
        return joist_continue(k, ch(src, 4) == 0 ? 0 : 1u + ch(src, 4));
    case JX_FAULT:
        return joist_fault(k, pick_thread(k, src, 0), 1u + ch(src, 5), step, ch(src, 16), ch(src, 16));
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
    /* A third of operations run with a small preemption limit (0016): any
     * destruction they start or cause stops after a few slots and is left
     * half done, for every other operation to meet, with every invariant
     * checked in between. */
    k->preempt = ch(src, 3) == 0 ? 1u + ch(src, 4) : 0;
    uint64_t w0 = k->work;
    why[0] = '\0';
    /* Every third operation runs with the run queues held, as the kernel
     * runs a system call, and settled after. */
    int held = st->steps % 3u == 0;
    if (held) {
        joist_sched_hold(k);
    }
    joist_err e = apply(k, src, op, st->steps, &ls, &bad, why, whylen);
    if (held) {
        joist_sched_settle(k);
    }
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
    /* Donation coverage: loans standing after this step, and chains. */
    int any_lent = 0;
    int any_chain = 0;
    for (uint32_t t = 0; t < JOIST_MAX_THREADS; t++) {
        if (k->th[t].state != JOIST_T_FREE && k->th[t].sc_to != 0) {
            any_lent = 1;
            any_chain |= k->th[k->th[t].sc_to - 1u].sc_to != 0;
        }
    }
    if (op == JX_FAULT) {
        st->faults_taken += (unsigned long)(e == JOIST_BLOCKED);
        st->faults_fatal += (unsigned long)(e == JOIST_E_PEER);
    }
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        if (k->obj[i].type == JOIST_OBJ_FRAME && k->obj[i].mapcount != 0) {
            st->mapped++;
            break;
        }
    }
    st->frames_dying += (unsigned long)(k->frames_dying != 0);
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        if (k->cs[s].pt_used != 0) {
            st->tables++;
            break;
        }
    }
    st->dying += (unsigned long)(k->npending != 0);
    st->dying_deep += (unsigned long)(k->npending > 1u);
    st->lent += (unsigned long)any_lent;
    st->chains += (unsigned long)any_chain;
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
    /* A fault's caller is resumed (r[0] == JOIST_FAULT_RESUME) or gone (0009). */
    int terminated = ls.faulting && ls.r0 != JOIST_FAULT_RESUME;
    if ((op == JX_SAVED || op == JX_REPLY) && e == JOIST_OK && terminated &&
        (ls.caller == 0 || k->th[ls.caller - 1u].state != JOIST_T_FREE)) {
        (void)snprintf(why, whylen, "0009: a fault's caller %u survived a terminate verdict",
                       ls.caller);
        return 0;
    }
    if (op == JX_SAVED && e == JOIST_OK && !terminated &&
        (ls.caller == 0 || k->th[ls.caller - 1u].state != JOIST_T_RUNNING ||
         k->th[ls.caller - 1u].result != JOIST_OK || k->th[ls.caller - 1u].faulting ||
         k->th[ls.caller - 1u].msg.r[0] != ls.r0)) {
        (void)snprintf(why, whylen, "I6: saved reply by thread %u missed caller %u", ls.t,
                       ls.caller);
        return 0;
    }
    if (op == JX_REPLY && e == JOIST_OK && !terminated &&
        (k->th[ls.t].reply_to != 0 || ls.caller == 0 ||
         k->th[ls.caller - 1u].state != JOIST_T_RUNNING ||
         k->th[ls.caller - 1u].result != JOIST_OK || k->th[ls.caller - 1u].faulting ||
         k->th[ls.caller - 1u].msg.r[0] != ls.r0)) {
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
    if (!hooks_agree(k, why, whylen)) {
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
    /* Finish any destruction left half done, then tear down unpreempted. */
    k->preempt = 0;
    if (joist_continue(k, 0) != JOIST_OK) {
        (void)snprintf(why, whylen, "0016: pending destruction didn't finish");
        return 0;
    }
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
        if (r->state <= JOIST_T_RUNNING || r->state == JOIST_T_SUSPENDED) {
            continue; /* a suspended thread is stopped, not waiting for anything */
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
    /* Frames lent to the root's own space stay until it dies, which it never
     * does (0021: no call gives lent pages back from a live space): dying,
     * still charged to the root. They are all that may be left. */
    uint32_t lent_root = 0;
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        const struct joist_object *o = &k->obj[i];
        lent_root += (uint32_t)(o->type == JOIST_OBJ_FRAME && o->dying && o->lent == root + 1u &&
                                o->donor == root);
    }
    if (k->objects_live != lent_root || accts != 0 || dns != 0 || spaces != 1 ||
        k->cs[root].quota + lent_root != k->budget || k->budget < boot_budget) {
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
