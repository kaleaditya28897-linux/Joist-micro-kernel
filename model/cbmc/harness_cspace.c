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

unsigned nondet_unsigned(void);
uint64_t nondet_u64(void);
struct joist_kernel nondet_kernel(void);

static struct joist_kernel k;

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
    __CPROVER_assume(joist_check_invariants(&k, NULL, 0) == 0);
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
        joist_err e;

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
        default:
            e = joist_task_destroy(&k, cs, a);
            break;
        }

#ifndef JOIST_CBMC_NO_INVARIANTS
        __CPROVER_assert(joist_check_invariants(&k, NULL, 0) == 0, "invariants hold");
#endif
        if (e == JOIST_OK && (op == 1 || op == 2 || op == 4)) {
            uint32_t dst = op == 4 ? to : cs;
            const struct joist_cap *s = joist__slot_c(&k, cs, a);
            const struct joist_cap *d = joist__slot_c(&k, dst, b);
            __CPROVER_assert(s != NULL && d != NULL &&
                                 (d->rights & (joist_rights)~s->rights) == 0,
                             "I8: rights never grow");
        }
    }
    return 0;
}
