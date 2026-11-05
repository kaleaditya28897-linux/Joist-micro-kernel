/* SPDX-License-Identifier: GPL-3.0-only */
/* test_names.c - glibc/sysdeps/joist/joist-names.c (Mach port names on
 * capability slots; Phase 5 step 7, items 2 and 3 of its design) against
 * the kernel model.
 *
 * The table's kernel calls are the model's, on the space the test says is
 * calling. Checked after every operation (names_agree): each name is what
 * the space holds in its slot (a send right a capability with SEND, a
 * receive right its badged send capability exactly while it has send
 * references, a port set or a receive right's own endpoint an endpoint,
 * free names, dead names and landing slots nothing), and no capability is
 * in a slot the table doesn't account for. A refused operation must change
 * neither the table nor the space. Directed tests follow libports' uses
 * and transfers between two tasks (no-senders included); the property test
 * runs seeded sequences of every operation. */
#include "harness.h"
#include "joist-names.h"
#include "joist/core.h"

#include <stdlib.h>
#include <string.h>

static struct joist_kernel k;
static joist_cspace A, B, cur; /* the two tasks; the one calling */
static struct jn_table TA, TB;
/* A's fixed slots: 1 B's task capability, 2 and 3 a notification and a
 * badged capability to signal it (for no-senders). */
enum { A_TASK_B = 1, A_NOTE = 2, A_SIG = 3, A_FIXED = 4, B_FIXED = 1 };

/* The environment: the model, on `cur`. */
static int kerr(joist_err e)
{
    return e == JOIST_OK ? 0 : e == JOIST_E_NOMEM ? JN_KERN_NOMEM : (int)e + 100;
}
int __joist_names_endpoint(uint32_t dst)
{
    return kerr(joist_obj_create(&k, cur, JOIST_OBJ_ENDPOINT, dst));
}
int __joist_names_mint(uint32_t src, uint32_t dst, uint64_t badge)
{
    return kerr(joist_cap_mint(&k, cur, src, dst, JOIST_R_SEND | JOIST_R_GRANT, badge));
}
int __joist_names_derive(uint32_t src, uint32_t dst)
{
    return kerr(joist_cap_derive(&k, cur, src, dst, JOIST_R_SEND | JOIST_R_GRANT));
}
int __joist_names_delete(uint32_t slot)
{
    return kerr(joist_cap_delete(&k, cur, slot));
}
int __joist_names_grow(uint32_t leaf)
{
    return kerr(joist_cspace_grow(&k, cur, leaf));
}
int __joist_names_shrink(uint32_t leaf)
{
    return kerr(joist_cspace_shrink(&k, cur, leaf));
}

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

/* The table and the space agree (the header comment). 0, or a line saying
 * which name is wrong. */
static int names_agree(const struct jn_table *t, joist_cspace cs, uint32_t fixed)
{
    for (uint32_t n = fixed; n < JN_NAMES; n++) {
        const struct jn_entry *e = &t->e[n];
        struct joist_cap c;
        joist_err r = joist_cap_read(&k, cs, n, &c);
        int leaf = (t->leaves >> (n / JN_LEAF)) & 1;
        if (r == JOIST_E_RANGE) {
            if (leaf || e->kind != JN_FREE) {
                return (int)__LINE__;
            }
            continue;
        }
        if (!leaf) {
            return (int)__LINE__; /* the space has a leaf the table thinks it hasn't */
        }
        int has = r == JOIST_OK;
        switch (e->kind) {
        case JN_FREE:
        case JN_DEAD:
        case JN_LANDING:
            if (has || (e->kind == JN_DEAD && e->refs == 0)) {
                return (int)__LINE__;
            }
            break;
        case JN_SEND:
            if (!has || !(c.rights & JOIST_R_SEND) || e->refs == 0) {
                return (int)__LINE__;
            }
            break;
        case JN_RECEIVE:
            if (has != (e->refs > 0)) {
                return (int)__LINE__;
            }
            if (has && (c.type != JOIST_OBJ_ENDPOINT || c.badge != e->badge || !e->minted ||
                        (c.rights & JOIST_R_RECV))) {
                return (int)__LINE__;
            }
            if (!e->minted && (e->refs != 0 || e->ep != 0)) {
                return (int)__LINE__;
            }
            if (e->ep != 0 && (e->set != 0 || t->e[e->ep].kind != JN_HIDDEN)) {
                return (int)__LINE__;
            }
            if (e->set != 0 && t->e[e->set].kind != JN_PORTSET) {
                return (int)__LINE__;
            }
            break;
        case JN_PORTSET:
        case JN_HIDDEN:
            if (!has || c.type != JOIST_OBJ_ENDPOINT || !(c.rights & JOIST_R_RECV) || c.badge != 0) {
                return (int)__LINE__;
            }
            break;
        default:
            return (int)__LINE__;
        }
        /* A badge names one port of its endpoint. */
        if (e->kind == JN_RECEIVE && e->minted) {
            for (uint32_t m = n + 1u; m < JN_NAMES; m++) {
                const struct jn_entry *f = &t->e[m];
                if (f->kind == JN_RECEIVE && f->minted && f->badge == e->badge && f->set == e->set &&
                    e->set != 0) {
                    return (int)__LINE__;
                }
            }
        }
    }
    return 0;
}

static void setup(void)
{
    joist_init(&k);
    if (joist_cspace_create(&k, 400, &A) != JOIST_OK ||
        joist_task_create(&k, A, 200, A_TASK_B) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 1);
    }
    B = k.obj[k.cs[A].slot[A_TASK_B].obj - 1u].target - 1u;
    __jn_init(&TA, A_FIXED);
    __jn_init(&TB, B_FIXED);
    cur = A;
}

static int agree(void)
{
    int a = names_agree(&TA, A, A_FIXED), b = names_agree(&TB, B, B_FIXED);
    if (a || b) {
        fprintf(stderr, "  names disagree: A at line %d, B at line %d\n", a, b);
    }
    return a == 0 && b == 0 && clean();
}

/* Move a capability A prepared to B's landing slot, as a message's
 * delivery does (cap_give: a copy with SEND and GRANT). */
static uint32_t give_to_b(uint32_t aslot)
{
    uint32_t land;
    cur = B;
    if (__jn_landing(&TB, &land) != JN_OK) {
        return 0;
    }
    cur = A;
    if (joist_cap_give(&k, A, A_TASK_B, aslot, land, JOIST_R_SEND | JOIST_R_GRANT) != JOIST_OK) {
        return 0;
    }
    cur = B;
    __jn_arrived(&TB, land);
    cur = A;
    return land;
}

JT_TEST(names_libports_port)
{
    setup();
    uint32_t set, port, type, refs;
    /* A bucket and a port in it, its payload a pointer (create-internal.c). */
    JT_REQUIRE(__jn_allocate(&TA, JN_RIGHT_PORT_SET, &set) == JN_OK);
    JT_REQUIRE(__jn_allocate(&TA, JN_RIGHT_RECEIVE, &port) == JN_OK);
    JT_CHECK(set >= A_FIXED && port != set);
    JT_CHECK(__jn_move_member(&TA, port, set) == JN_OK);
    JT_CHECK(__jn_set_payload(&TA, port, 0x7f0012345670ull) == JN_OK);
    JT_CHECK(__jn_recv_slot(&TA, port) == set && __jn_recv_slot(&TA, set) == set);
    JT_CHECK(__jn_badge(&TA, port) == 0); /* none before its first send right */
    JT_CHECK(agree());
    /* ports_get_right: a send right under its own name. */
    JT_CHECK(__jn_insert_right(&TA, port, port, JN_MAKE_SEND) == JN_OK);
    JT_CHECK(__jn_badge(&TA, port) == 0x7f0012345670ull);
    JT_CHECK(__jn_type(&TA, port, &type) == JN_OK && type == (JN_TYPE_SEND | JN_TYPE_RECEIVE));
    JT_CHECK(__jn_get_refs(&TA, port, JN_RIGHT_SEND, &refs) == JN_OK && refs == 1);
    JT_CHECK(agree());
    /* 0005: fixed now. */
    uint32_t set2;
    JT_REQUIRE(__jn_allocate(&TA, JN_RIGHT_PORT_SET, &set2) == JN_OK);
    JT_CHECK(__jn_move_member(&TA, port, set2) == JN_INVALID_RIGHT);
    JT_CHECK(__jn_move_member(&TA, port, set) == JN_OK); /* where it is: no change */
    JT_CHECK(__jn_set_payload(&TA, port, 0x1000) == JN_INVALID_RIGHT);
    JT_CHECK(__jn_set_payload(&TA, port, 0x7f0012345670ull) == JN_OK);
    /* Its send right gone: the capability goes, the receive right stays. */
    JT_CHECK(__jn_deallocate(&TA, port) == JN_OK);
    JT_CHECK(__jn_type(&TA, port, &type) == JN_OK && type == JN_TYPE_RECEIVE);
    JT_CHECK(__jn_deallocate(&TA, port) == JN_INVALID_RIGHT);
    JT_CHECK(agree());
    /* And again, the same badge. */
    JT_CHECK(__jn_insert_right(&TA, port, port, JN_MAKE_SEND) == JN_OK);
    JT_CHECK(__jn_mod_refs(&TA, port, JN_RIGHT_SEND, 4) == JN_OK);
    JT_CHECK(__jn_get_refs(&TA, port, JN_RIGHT_SEND, &refs) == JN_OK && refs == 5);
    JT_CHECK(__jn_mod_refs(&TA, port, JN_RIGHT_SEND, -6) == JN_INVALID_VALUE);
    JT_CHECK(__jn_badge(&TA, port) == 0x7f0012345670ull);
    JT_CHECK(agree());
    /* The receive right destroyed with send references: a dead name. */
    JT_CHECK(__jn_mod_refs(&TA, port, JN_RIGHT_RECEIVE, -1) == JN_OK);
    JT_CHECK(__jn_type(&TA, port, &type) == JN_OK && type == JN_TYPE_DEAD_NAME);
    JT_CHECK(__jn_get_refs(&TA, port, JN_RIGHT_DEAD_NAME, &refs) == JN_OK && refs == 5);
    JT_CHECK(__jn_mod_refs(&TA, port, JN_RIGHT_DEAD_NAME, -5) == JN_OK);
    JT_CHECK(__jn_type(&TA, port, &type) == JN_INVALID_NAME);
    JT_CHECK(__jn_destroy(&TA, set) == JN_OK && __jn_destroy(&TA, set2) == JN_OK);
    JT_CHECK(agree());
}

JT_TEST(names_transfer_and_nosenders)
{
    setup();
    uint32_t set, port, slot;
    int temp;
    JT_REQUIRE(__jn_allocate(&TA, JN_RIGHT_PORT_SET, &set) == JN_OK);
    JT_REQUIRE(__jn_allocate(&TA, JN_RIGHT_RECEIVE, &port) == JN_OK);
    JT_REQUIRE(__jn_move_member(&TA, port, set) == JN_OK);
    /* No-senders on the set's endpoint, bound to a notification of A's. */
    JT_REQUIRE(joist_obj_create(&k, A, JOIST_OBJ_NOTIFICATION, A_NOTE) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, A, A_NOTE, A_SIG, JOIST_R_SEND, 1) == JOIST_OK);
    JT_REQUIRE(joist_bind_nosenders(&k, A, set, A_SIG) == JOIST_OK);
    /* A message with MAKE_SEND: a slot for it alone, deleted after. */
    JT_CHECK(__jn_send_prepare(&TA, port, JN_MAKE_SEND, &slot, &temp) == JN_OK && temp == 1);
    uint32_t got = give_to_b(slot);
    JT_REQUIRE(got != 0);
    __jn_send_done(&TA, port, JN_MAKE_SEND, slot, temp, 1);
    uint64_t badge = __jn_badge(&TA, port);
    JT_CHECK(badge >= (1ull << 63)); /* no payload: the counter's */
    struct joist_cap c;
    JT_CHECK(joist_cap_read(&k, B, got, &c) == JOIST_OK && c.badge == badge);
    /* MOVE_SEND gives up one reference once sent, none if not. */
    cur = A;
    uint32_t mv;
    JT_REQUIRE(__jn_insert_right(&TA, port, port, JN_MAKE_SEND) == JN_OK);
    JT_REQUIRE(__jn_mod_refs(&TA, port, JN_RIGHT_SEND, 1) == JN_OK);
    JT_CHECK(__jn_send_prepare(&TA, port, JN_MOVE_SEND, &mv, &temp) == JN_OK && mv == port && temp == 0);
    __jn_send_done(&TA, port, JN_MOVE_SEND, mv, temp, 0);
    uint32_t r2;
    JT_CHECK(__jn_get_refs(&TA, port, JN_RIGHT_SEND, &r2) == JN_OK && r2 == 2);
    __jn_send_done(&TA, port, JN_MOVE_SEND, mv, temp, 1);
    JT_CHECK(__jn_get_refs(&TA, port, JN_RIGHT_SEND, &r2) == JN_OK && r2 == 1);
    JT_CHECK(__jn_deallocate(&TA, port) == JN_OK);
    JT_CHECK(agree());
    /* B's own name for it, a copy under another name, a move back. */
    uint32_t type, refs;
    cur = B;
    JT_CHECK(__jn_type(&TB, got, &type) == JN_OK && type == JN_TYPE_SEND);
    JT_CHECK(__jn_insert_right(&TB, got, got, JN_COPY_SEND) == JN_OK);
    JT_CHECK(__jn_get_refs(&TB, got, JN_RIGHT_SEND, &refs) == JN_OK && refs == 2);
    uint32_t other = got + 3u;
    JT_CHECK(__jn_insert_right(&TB, other, got, JN_COPY_SEND) == JN_OK);
    JT_CHECK(__jn_insert_right(&TB, other, got, JN_COPY_SEND) == JN_NAME_EXISTS);
    JT_CHECK(agree());
    cur = A;
    JT_CHECK(agree());
    /* Every sender in B gone: the badge is queued on A's set, once. */
    cur = B;
    JT_CHECK(__jn_deallocate(&TB, got) == JN_OK);
    JT_CHECK(__jn_destroy(&TB, other) == JN_OK);
    joist_badge q = 0;
    JT_CHECK(joist_nosenders_next(&k, A, set, &q) == JOIST_OK && q == 0);
    JT_CHECK(__jn_deallocate(&TB, got) == JN_OK);
    JT_CHECK(joist_nosenders_next(&k, A, set, &q) == JOIST_OK && q == badge);
    JT_CHECK(joist_nosenders_next(&k, A, set, &q) == JOIST_OK && q == 0);
    cur = A;
    JT_CHECK(agree());
    /* A's own send right doesn't hold no-senders off once given up. */
    JT_CHECK(__jn_insert_right(&TA, port, port, JN_MAKE_SEND) == JN_OK);
    JT_CHECK(__jn_deallocate(&TA, port) == JN_OK);
    JT_CHECK(joist_nosenders_next(&k, A, set, &q) == JOIST_OK && q == badge);
    JT_CHECK(agree());
}

JT_TEST(names_standalone_port)
{
    setup();
    uint32_t port, slot;
    int temp;
    JT_REQUIRE(__jn_allocate(&TA, JN_RIGHT_RECEIVE, &port) == JN_OK);
    JT_CHECK(__jn_recv_slot(&TA, port) == 0); /* made at its first send right */
    JT_CHECK(__jn_send_prepare(&TA, port, JN_MAKE_SEND, &slot, &temp) == JN_OK);
    uint32_t ep = __jn_recv_slot(&TA, port);
    JT_CHECK(ep != 0 && TA.e[ep].kind == JN_HIDDEN);
    uint32_t got = give_to_b(slot);
    JT_REQUIRE(got != 0);
    __jn_send_done(&TA, port, JN_MAKE_SEND, slot, temp, 1);
    JT_CHECK(agree());
    /* Its receive right destroyed: its own endpoint dies, B's calls fail. */
    JT_CHECK(__jn_destroy(&TA, port) == JN_OK);
    JT_CHECK(TA.e[ep].kind == JN_FREE);
    struct joist_cap c;
    JT_CHECK(joist_cap_read(&k, B, got, &c) == JOIST_OK);
    JT_CHECK(k.obj[c.obj - 1u].type == JOIST_OBJ_ENDPOINT && k.obj[c.obj - 1u].dead);
    JT_CHECK(agree());
}

JT_TEST(names_refusals)
{
    setup();
    uint32_t r, s, refs, slot;
    int temp;
    JT_REQUIRE(__jn_allocate(&TA, JN_RIGHT_RECEIVE, &r) == JN_OK);
    JT_REQUIRE(__jn_allocate(&TA, JN_RIGHT_PORT_SET, &s) == JN_OK);
    JT_CHECK(__jn_allocate(&TA, JN_RIGHT_SEND, &slot) == JN_INVALID_VALUE);
    JT_CHECK(__jn_deallocate(&TA, 0) == JN_INVALID_NAME);
    JT_CHECK(__jn_deallocate(&TA, JN_NAMES) == JN_INVALID_NAME);
    JT_CHECK(__jn_deallocate(&TA, s) == JN_INVALID_RIGHT);
    JT_CHECK(__jn_mod_refs(&TA, r, JN_RIGHT_SEND, 1) == JN_INVALID_RIGHT); /* no send right */
    JT_CHECK(__jn_mod_refs(&TA, r, JN_RIGHT_RECEIVE, 1) == JN_INVALID_VALUE);
    JT_CHECK(__jn_mod_refs(&TA, s, JN_RIGHT_RECEIVE, -1) == JN_INVALID_RIGHT);
    JT_CHECK(__jn_move_member(&TA, s, s) == JN_INVALID_RIGHT);
    JT_CHECK(__jn_move_member(&TA, r, r) == JN_INVALID_RIGHT);
    JT_CHECK(__jn_insert_right(&TA, r + 1u, r, JN_MAKE_SEND) == JN_INVALID_VALUE);
    JT_CHECK(__jn_insert_right(&TA, r + 1u, r, JN_COPY_SEND) == JN_INVALID_RIGHT);
    JT_CHECK(__jn_send_prepare(&TA, s, JN_MAKE_SEND, &slot, &temp) == JN_INVALID_RIGHT);
    JT_CHECK(__jn_set_payload(&TA, r, 1ull << 63) == JN_INVALID_RIGHT); /* the counter's range */
    /* References at their limit. */
    JT_REQUIRE(__jn_insert_right(&TA, r, r, JN_MAKE_SEND) == JN_OK);
    JT_CHECK(__jn_mod_refs(&TA, r, JN_RIGHT_SEND, (int)JN_MAX_REFS) == JN_UREFS_OVERFLOW);
    JT_CHECK(__jn_mod_refs(&TA, r, JN_RIGHT_SEND, (int)JN_MAX_REFS - 1) == JN_OK);
    JT_CHECK(__jn_insert_right(&TA, r, r, JN_MAKE_SEND) == JN_UREFS_OVERFLOW);
    JT_CHECK(__jn_get_refs(&TA, r, JN_RIGHT_SEND, &refs) == JN_OK && refs == JN_MAX_REFS);
    JT_CHECK(agree());
    /* Fixed names: references not counted, never deleted. */
    JT_CHECK(__jn_deallocate(&TA, A_TASK_B) == JN_OK && __jn_destroy(&TA, A_TASK_B) == JN_OK);
    struct joist_cap c;
    JT_CHECK(joist_cap_read(&k, A, A_TASK_B, &c) == JOIST_OK);
    JT_CHECK(agree());
}

JT_TEST(names_fill_the_space)
{
    setup();
    /* Every name taken, the second leaf grown on the way; then none. */
    uint32_t n, count = 0;
    while (__jn_allocate(&TA, JN_RIGHT_DEAD_NAME, &n) == JN_OK) {
        count++;
    }
    JT_CHECK(count == JN_NAMES - A_FIXED);
    JT_CHECK(TA.leaves == (1ull << JN_LEAVES) - 1u);
    JT_CHECK(__jn_allocate(&TA, JN_RIGHT_RECEIVE, &n) == JN_NO_SPACE);
    JT_CHECK(agree());
    JT_CHECK(__jn_deallocate(&TA, 70) == JN_OK);
    JT_CHECK(__jn_allocate(&TA, JN_RIGHT_RECEIVE, &n) == JN_OK && n == 70);
    JT_CHECK(agree());
}

/* The property test: seeded sequences of every operation on A (and
 * transfers to B), the agreement checked after each, a refusal checked to
 * change nothing. */
static uint64_t rng;
static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static struct jn_table snap_t;
static struct joist_cap snap_c[JN_NAMES];
static joist_err snap_r[JN_NAMES];
static void snapshot(void)
{
    snap_t = TA;
    for (uint32_t n = 0; n < JN_NAMES; n++) {
        snap_r[n] = joist_cap_read(&k, A, n, &snap_c[n]);
    }
}
static int unchanged(void)
{
    if (memcmp(&snap_t, &TA, sizeof TA) != 0) {
        return 0;
    }
    for (uint32_t n = 0; n < JN_NAMES; n++) {
        struct joist_cap c;
        joist_err r = joist_cap_read(&k, A, n, &c);
        if (r != snap_r[n] || (r == JOIST_OK && memcmp(&c, &snap_c[n], sizeof c) != 0)) {
            return 0;
        }
    }
    return 1;
}

#define SEEDS 300u
#define STEPS 400u

JT_TEST(names_property)
{
    unsigned refused = 0, done = 0;
    for (unsigned seed = 1; seed <= SEEDS; seed++) {
        setup();
        rng = seed * 0x9e3779b97f4a7c15ull;
        for (unsigned step = 0; step < STEPS; step++) {
            uint32_t name = (uint32_t)(rnd() % (JN_NAMES + 2u));
            uint32_t other = (uint32_t)(rnd() % JN_NAMES);
            int delta = (int)(rnd() % 5u) - 2;
            static const int rights[] = {JN_RIGHT_SEND, JN_RIGHT_RECEIVE, JN_RIGHT_PORT_SET,
                                         JN_RIGHT_DEAD_NAME, 7};
            static const int disps[] = {JN_MAKE_SEND, JN_COPY_SEND, JN_MOVE_SEND, 3};
            int right = rights[rnd() % 5u];
            int disp = disps[rnd() % 4u];
            /* Bias towards names in use. */
            if (rnd() % 2u) {
                for (uint32_t i = 0; i < 16u; i++) {
                    uint32_t c = A_FIXED + (uint32_t)(rnd() % (JN_NAMES - A_FIXED));
                    uint8_t kd = TA.e[c].kind;
                    if (kd != JN_FREE && kd != JN_HIDDEN && kd != JN_LANDING) {
                        name = c;
                        break;
                    }
                }
            }
            int r = JN_OK, op = (int)(rnd() % 12u);
            uint32_t out, slot;
            int temp;
            cur = A;
            snapshot();
            switch (op) {
            case 0:
            case 1:
                r = __jn_allocate(&TA, right, &out);
                break;
            case 2:
                r = __jn_deallocate(&TA, name);
                break;
            case 3:
                r = __jn_mod_refs(&TA, name, right, delta);
                break;
            case 4:
                r = __jn_destroy(&TA, name);
                break;
            case 5:
                r = __jn_insert_right(&TA, rnd() % 2u ? name : other, name, disp);
                break;
            case 6:
                r = __jn_move_member(&TA, name, rnd() % 3u ? other : 0);
                break;
            case 7:
                r = __jn_set_payload(&TA, name, rnd() % 4u ? 0x1000u + rnd() % 64u : 0);
                break;
            case 8:
            case 9:
                r = __jn_send_prepare(&TA, name, disp, &slot, &temp);
                if (r == JN_OK) {
                    int sent = (int)(rnd() % 4u) != 0;
                    if (sent && give_to_b(slot) == 0) {
                        sent = 0;
                    }
                    __jn_send_done(&TA, name, disp, slot, temp, sent);
                }
                break;
            default: /* B gives up one of what it got */
                cur = B;
                (void)__jn_deallocate(&TB, B_FIXED + (uint32_t)(rnd() % (JN_NAMES - B_FIXED)));
                cur = A;
                break;
            }
            if (r != JN_OK && op <= 9) {
                refused++;
                if (!unchanged()) {
                    fprintf(stderr, "  seed %u step %u: op %d refused (%d) but changed something\n", seed,
                            step, op, r);
                    jt_fail(__FILE__, __LINE__, "a refusal changed nothing", 1);
                    return;
                }
            } else {
                done++;
            }
            if (!agree()) {
                fprintf(stderr, "  seed %u step %u: op %d (result %d)\n", seed, step, op, r);
                jt_fail(__FILE__, __LINE__, "names agree with the space", 1);
                return;
            }
        }
    }
    fprintf(stderr, "  names: %u operations done, %u refused\n", done, refused);
    JT_CHECK(done > 1000u && refused > 1000u);
}
