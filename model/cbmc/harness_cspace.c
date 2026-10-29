/* harness_cspace.c - CBMC harness for the capability space (plan §8.1;
 * Phase 1, follow-on to work item 8). Run by `make cbmc` (cbmc/run.sh).
 *
 * Two modes. With JOIST_CBMC_INDUCTIVE (what `make cbmc` runs), a
 * preservation proof: from *any* kernel state joist_check_invariants accepts,
 * one operation with arbitrary arguments, at tiny table sizes. Without it,
 * every sequence of JOIST_CBMC_STEPS operations from boot. Either way every
 * argument is nondeterministic (out-of-range values included). After each
 * step it asserts:
 *
 *   - joist_check_invariants() finds nothing (I1, I3, I4, I5, I8 structure,
 *     task and quota conservation, 0013);
 *   - I8 directly: a derived, minted or granted capability has no right its
 *     source lacks.
 *
 * ("A failed operation changes nothing" is left to the property tests, which
 * compare the whole state against shadow models after every step; a
 * byte-wise comparison of the kernel struct costs CBMC more than everything
 * else here.)
 *
 * CBMC also checks every array access, pointer and shift on the way, and
 * --unwinding-assertions makes it fail if any loop needed more unwinding than
 * it was given, so a pass is a proof up to the bound. */
#include "internal.h"
#include "joist/core.h"

#include <stddef.h>

#ifndef JOIST_CBMC_STEPS
#define JOIST_CBMC_STEPS 3
#endif

unsigned joist__cbmc_line; /* internal.h: the last invariant check that fired */
unsigned nondet_unsigned(void);
uint64_t nondet_u64(void);
struct joist_kernel nondet_kernel(void);

static struct joist_kernel k;
static uint64_t query[JOIST_QUERY_MAX]; /* map_query's answer (op 16) */

/* Physical memory, as far as the core sees it (0020, 0021): the pages that
 * hooks.page reaches, holding the page tables' nodes. A node uses only its
 * first JOIST_PT_ENTRIES words (run.sh builds them small), so a page here is
 * that many; the hardware tables paired with the nodes are the kernel's
 * (kernel/mem/vm.c) and never read by the core. */
#ifndef JOIST_CBMC_SIM_PAGES
#define JOIST_CBMC_SIM_PAGES 8u
#endif
#define SIM_BASE 16u /* the first page number simulated */
static uint64_t sim[JOIST_CBMC_SIM_PAGES][JOIST_PT_ENTRIES];

static void *sim_page(void *ctx, uint64_t pfn)
{
    (void)ctx;
    return pfn >= SIM_BASE && pfn - SIM_BASE < JOIST_CBMC_SIM_PAGES ? sim[pfn - SIM_BASE] : NULL;
}

#ifdef JOIST_CBMC_TABLES
/* Every page a live space's tables hold, as a node (its shadow and its
 * hardware page) or on its free list, is held once. The kernel makes it so
 * (take_page hands out each lent page once), but I12's check doesn't look,
 * since that takes storage or time growing with the tables; here, where
 * the tables are tiny, it is assumed of the state before and asserted of
 * the one after. A page is marked as it is met: met twice, or outside the
 * simulated memory (where no lent page is), it fails. Frames don't overlap
 * (I12), so one set of marks serves every space. */
_Static_assert(JOIST_PT_LEVELS == 3u, "the walk below is written for three levels");
static unsigned char held[JOIST_CBMC_SIM_PAGES];

static int hold(uint64_t pfn)
{
    if (pfn < SIM_BASE || pfn - SIM_BASE >= JOIST_CBMC_SIM_PAGES || held[pfn - SIM_BASE]) {
        return 0;
    }
    held[pfn - SIM_BASE] = 1;
    return 1;
}

static int pages_distinct(void)
{
    __CPROVER_array_set(held, 0);
    int ok = 1;
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        const struct joist_cspace_state *st = &k.cs[s];
        if (!st->live) {
            continue;
        }
        for (unsigned t2 = 0; t2 < JOIST_PT_TOP; t2++) {
            uint64_t e2 = st->pt_top[t2];
            if (e2 == 0) {
                continue;
            }
            ok &= hold(JOIST_PT_SHADOW(e2)) & hold(JOIST_PT_HW(e2));
            const uint64_t *n2 = sim_page(NULL, JOIST_PT_SHADOW(e2));
            for (unsigned t1 = 0; n2 != NULL && t1 < JOIST_PT_ENTRIES; t1++) {
                uint64_t e1 = n2[t1];
                if (e1 == 0) {
                    continue;
                }
                ok &= hold(JOIST_PT_SHADOW(e1)) & hold(JOIST_PT_HW(e1));
                const uint64_t *n1 = sim_page(NULL, JOIST_PT_SHADOW(e1));
                for (unsigned t0 = 0; n1 != NULL && t0 < JOIST_PT_ENTRIES; t0++) {
                    uint64_t e0 = n1[t0];
                    if (e0 != 0) {
                        ok &= hold(JOIST_PT_SHADOW(e0)) & hold(JOIST_PT_HW(e0));
                    }
                }
            }
        }
        uint64_t p = st->pt_free;
        for (unsigned i = 0; p != 0 && i <= JOIST_CBMC_SIM_PAGES; i++) { /* cbmc: free walk */
            ok &= hold(p - 1u);
            const uint64_t *fp = sim_page(NULL, p - 1u);
            p = fp != NULL ? fp[0] : 0;
        }
    }
    return ok;
}
#endif

static uint32_t any(uint32_t n)
{
    uint32_t v = nondet_unsigned();
    __CPROVER_assume(v <= n); /* n itself is one past a valid index */
    return v;
}

int main(void)
{
#ifdef JOIST_CBMC_INDUCTIVE
    /* Preservation: any state the invariant check accepts, with nothing
     * pending, then one operation (JOIST_CBMC_STEPS = 1). A counterexample
     * is either a bug or a missing invariant: a state the check accepts that
     * no execution reaches. */
    k = nondet_kernel();
    /* The host model has no kernel hooks (struct joist_hooks): an arbitrary
     * state would otherwise call arbitrary function pointers. The kernel's
     * hooks are tested by the executor's recorded shadow and by booting.
     * With page tables (JOIST_CBMC_TABLES) the page hook is the harness's:
     * the simulated memory above, of any contents. Without, it stays NULL:
     * no table page is reached there, and CBMC proves it (a call through a
     * pointer with no target is a failed check under --pointer-check), at a
     * fraction of the cost of the simulated memory. */
    k.hooks = (struct joist_hooks){0};
#ifdef JOIST_CBMC_TABLES
    k.hooks.page = sim_page;
    __CPROVER_havoc_object(sim);
    /* Page tables, lent pages and mappings of any shape the check accepts,
     * in frames lent within the simulated memory (what space_donate checks
     * of the frames it takes: their first and last pages reachable). */
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        const struct joist_object *f = &k.obj[o];
        __CPROVER_assume(f->lent == 0 ||
                         (f->base >= SIM_BASE && f->npages <= JOIST_CBMC_SIM_PAGES &&
                          f->base - SIM_BASE <= JOIST_CBMC_SIM_PAGES - f->npages));
    }
    __CPROVER_assume(pages_distinct());
#else
    /* No page tables, lent pages or mappings (0020, 0021): the operations
     * proved this way touch them only through a condemned frame's sweep,
     * proved with the tables (JOIST_CBMC_TABLES, the mapping operations). */
    for (uint32_t s = 0; s < JOIST_MAX_CSPACES; s++) {
        struct joist_cspace_state *st = &k.cs[s];
        for (unsigned i = 0; i < JOIST_PT_TOP; i++) {
            st->pt_top[i] = 0;
        }
        st->lent_first = 0;
        st->pt_free = 0;
        st->pt_avail = 0;
        st->pt_used = 0;
        st->tsweep = 0;
    }
    for (uint32_t o = 0; o < JOIST_MAX_OBJECTS; o++) {
        k.obj[o].mapcount = 0;
        k.obj[o].lent = 0;
        k.obj[o].lent_next = 0;
        k.obj[o].water = 0;
    }
    k.frames_dying = 0;
    k.fsweep = 0;
    k.fround = 0;
#endif
    __CPROVER_assume(joist_check_invariants(&k, NULL, 0) == 0);
    /* The place-in-line counter (64 bits, one step per wake-up or turn)
     * hasn't wrapped: that takes 2^64 steps, centuries at one a nanosecond.
     * An assumption of the proofs, not a check of the kernel's. */
    __CPROVER_assume(k.ready_seq < UINT64_MAX / 2u);
#ifdef JOIST_CBMC_NO_BUFFERS
    /* 0025's receiving and waiting proofs (cbmc/run.sh): no thread has an
     * IPC buffer, so no message words are copied (512 words would not fit
     * the formula in memory). Buffers are the unit and property tests'. */
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        __CPROVER_assume(k.th[i].ipc_buf == 0);
    }
#endif
#else
    joist_cspace root;
    joist_init(&k);
    __CPROVER_assert(joist_boot(&k, 6, &root) == JOIST_OK, "boot");
#endif

    for (unsigned step = 0; step < JOIST_CBMC_STEPS; step++) {
#ifdef JOIST_CBMC_OP
        uint32_t op = JOIST_CBMC_OP;
#else
        uint32_t op = any(8);
#endif
        uint32_t cs = any(JOIST_MAX_CSPACES);
        uint32_t to = any(JOIST_MAX_CSPACES);
        uint32_t a = any(JOIST_CSPACE_SLOTS);
        uint32_t b = any(JOIST_CSPACE_SLOTS);
        joist_rights mask = (joist_rights)any(15);
        joist_badge badge = nondet_u64();
        uint32_t done = 0;
        uint32_t skipped = 0;
        /* Pages, offsets and permissions for the mapping calls, any value. */
        uint64_t off = nondet_u64();
        uint64_t n = nondet_u64();
        uint64_t va = nondet_u64();
        uint64_t off2 = nondet_u64();
        unsigned perms = nondet_unsigned();
        joist_err e;

#ifdef JOIST_CBMC_SENDERS
        /* Split a receive's proof (cbmc/run.sh, OP/s and OP/n): slot a of
         * space 0 is an endpoint with callers queued (1), or it isn't (0).
         * The two cover every case. */
        {
            const struct joist_cap *q = joist__slot_c(&k, 0, a);
            int senders = q != NULL && q->obj != 0 && q->type == JOIST_OBJ_ENDPOINT &&
                          k.obj[q->obj - 1u].qkind == JOIST_Q_SENDERS;
            __CPROVER_assume(senders == JOIST_CBMC_SENDERS);
        }
#endif
#ifdef JOIST_CBMC_SLOT_TYPE
        /* Split one operation's proof by the type of the capability in slot
         * a (JOIST_OBJ_NONE also covers out-of-range and empty slots), so
         * each piece is a smaller problem; together the pieces cover every
         * case. */
        const struct joist_cap *sa = cs < JOIST_MAX_CSPACES ? joist__slot_c(&k, cs, a) : NULL;
        if (JOIST_CBMC_SLOT_TYPE == JOIST_OBJ_NONE) {
            __CPROVER_assume(sa == NULL || sa->obj == 0);
        } else {
            __CPROVER_assume(sa != NULL && sa->obj != 0 && sa->type == JOIST_CBMC_SLOT_TYPE);
        }
#endif

        switch (op) {
        case 0:
            e = joist_obj_create(&k, cs, (joist_obj_type)any(JOIST_OBJ_REPLY), b);
            break;
        case 1:
            e = joist_cap_derive(&k, cs, a, b, mask);
            break;
        case 2:
            e = joist_cap_mint(&k, cs, a, b, mask, badge);
            break;
        case 3:
            e = joist_cap_delete(&k, cs, a);
            break;
        case 4:
            e = joist__cap_grant(&k, cs, a, to, b, mask);
            break;
        case 5:
            e = joist_task_create(&k, cs, any(3), b);
            break;
        case 7:
            e = joist_cspace_grow(&k, cs, any(JOIST_CNODE_ROOT));
            break;
        case 8:
            e = joist_cspace_shrink(&k, cs, any(JOIST_CNODE_ROOT));
            break;
        /* Decision 0022. Not in the Makefile's CBMC_OPS yet: run them with
         * `make cbmc CBMC_OPS="9 10"`. The batch's loop is bounded by the
         * table here (JOIST_CSPACE_SLOTS), whatever count says. */
        case 9:
            e = joist_obj_kill(&k, cs, a);
            break;
        case 10:
            e = joist_cap_copy_batch(&k, cs, a, b, any(JOIST_COPY_BATCH + 1u), mask,
                                     any(JOIST_COPY_RECV + 1u), &done, &skipped);
            break;
        /* Decisions 0020 and 0021, with the page tables (JOIST_CBMC_TABLES):
         * a is the frame (or for unmap_range and protect the task), b the
         * task (or for space_donate the frame), to the old frame of a
         * conditional map. */
        case 11:
            e = joist_map(&k, cs, a, off, n, b, va, perms);
            break;
        case 12:
            e = joist_map_replace(&k, cs, a, off, n, b, va, perms, to, off2);
            break;
        case 13:
            e = joist_unmap_range(&k, cs, a, va, n);
            break;
        case 14:
            e = joist_protect(&k, cs, a, va, n, perms);
            break;
        case 15:
            e = joist_space_donate(&k, cs, a, b);
            break;
        case 16:
            /* map_query (0020, amended): b the frame named, or none. */
            if (any(1) == 0) {
                b = JOIST_SLOT_NONE;
            }
            e = joist_map_query(&k, cs, a, va, n, b, query);
            break;
        /* Decision 0025, the fallback context: the calls that bind and
         * unbind one, and those whose paths now take or drop it (receiving
         * and waiting give a loan back; a timeout wakes a thread). Run with
         * two threads (cbmc/run.sh), so a loan can stand. */
        case 17:
            e = joist_sc_bind_fallback(&k, cs, a, b);
            break;
        case 18:
            e = joist_sc_unbind(&k, cs, a);
            break;
        case 19:
            e = joist_recv(&k, any(JOIST_MAX_THREADS), a, any(1) == 0 ? 0 : nondet_u64());
            break;
        case 20:
            e = joist_wait(&k, any(JOIST_MAX_THREADS), a, any(1) == 0 ? 0 : nondet_u64());
            break;
        case 21:
            joist_tick(&k, nondet_u64());
            e = JOIST_OK;
            break;
        default:
            e = joist_task_destroy(&k, cs, a);
            break;
        }

#ifndef JOIST_CBMC_NO_INVARIANTS
        __CPROVER_assert(joist_check_invariants(&k, NULL, 0) == 0, "invariants hold");
        /* (Read, so the trace keeps it: the line of the check that fired.) */
        __CPROVER_assert(joist__cbmc_line == 0, "no invariant check fired");
#endif
#if defined(JOIST_CBMC_TABLES) && defined(JOIST_CBMC_INDUCTIVE)
        __CPROVER_assert(pages_distinct(), "I12: every table page held once");
#endif
        if (e == JOIST_OK && (op == 1 || op == 2 || op == 4)) {
            uint32_t dst = op == 4 ? to : cs;
            const struct joist_cap *s = joist__slot_c(&k, cs, a);
            const struct joist_cap *d = joist__slot_c(&k, dst, b);
            __CPROVER_assert(s != NULL && d != NULL &&
                                 (d->rights & (joist_rights)~s->rights) == 0,
                             "I8: rights never grow");
        }
        if (e == JOIST_OK && (op == 11 || op == 12) && (perms & JOIST_MAP_W) != 0) {
            const struct joist_cap *fc = joist__slot_c(&k, cs, a);
            __CPROVER_assert(fc != NULL && (fc->rights & JOIST_R_WRITE) != 0,
                             "I8: a page is writable only through a frame capability that writes");
        }
        if (e == JOIST_OK && op == 16) {
            /* A frame is reported only through a frame capability with
             * CONTROL; the query changed nothing (the invariants above). */
            const struct joist_cap *fc = b == JOIST_SLOT_NONE ? NULL : joist__slot_c(&k, cs, b);
            for (uint64_t i = 0; i < n && i < JOIST_QUERY_MAX; i++) {
                __CPROVER_assert((query[i] & JOIST_QUERY_FRAME) == 0 ||
                                     (fc != NULL && fc->type == JOIST_OBJ_FRAME &&
                                      (fc->rights & JOIST_R_CONTROL) != 0),
                                 "0020: map_query names only a frame the caller controls");
            }
        }
        if (op == 10 && done != 0) {
            /* I8 for each copy the batch made: slot b + i of the task named
             * by slot a. */
            const struct joist_cap *tc = joist__slot_c(&k, cs, a);
            uint32_t child = k.obj[tc->obj - 1u].target - 1u;
            for (uint32_t i = 0; i < done; i++) {
                const struct joist_cap *s = joist__slot_c(&k, cs, b + i);
                const struct joist_cap *d = joist__slot_c(&k, child, b + i);
                __CPROVER_assert(s == NULL || s->obj == 0 || (skipped >> i & 1u) != 0 ||
                                     (d != NULL && d->obj == s->obj &&
                                      (d->rights & (joist_rights)~s->rights) == 0),
                                 "I8: a batch copy has no right its source lacks");
            }
        }
    }
    return 0;
}
