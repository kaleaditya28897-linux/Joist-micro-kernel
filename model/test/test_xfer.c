/* SPDX-License-Identifier: GPL-3.0-only */
/* test_xfer.c - unit tests for capability transfer in messages (Phase 3
 * step 3; docs/abi.md, closing its open item 1). Two spaces: the server's,
 * holding the endpoint, and the client's, holding a send capability to it. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

static struct joist_kernel k;
static joist_cspace scs, ccs; /* server and client spaces */
static joist_thread srv, cli;

/* Server space: EP (all rights), EP_RECV. Client space: EP_SEND (badge 77),
 * NOTE (a notification with all rights), BARE (a notification capability
 * without GRANT). */
enum { EP = 0, EP_RECV = 1, EP_SEND = 0, NOTE = 1, BARE = 2 };

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

static void setup(void)
{
    joist_init(&k);
    if (joist_cspace_create(&k, 16, &scs) != JOIST_OK ||
        joist_cspace_create(&k, 16, &ccs) != JOIST_OK ||
        joist_obj_create(&k, scs, JOIST_OBJ_ENDPOINT, EP) != JOIST_OK ||
        joist_cap_derive(&k, scs, EP, EP_RECV, JOIST_R_RECV) != JOIST_OK ||
        /* The client's badged send capability, made through a temporary
         * full copy (mint needs GRANT on its source). */
        joist__cap_grant(&k, scs, EP, ccs, 9, JOIST_R_ALL) != JOIST_OK ||
        joist_cap_mint(&k, ccs, 9, EP_SEND, JOIST_R_SEND, 77) != JOIST_OK ||
        joist_cap_delete(&k, ccs, 9) != JOIST_OK ||
        joist_obj_create(&k, ccs, JOIST_OBJ_NOTIFICATION, NOTE) != JOIST_OK ||
        joist_cap_derive(&k, ccs, NOTE, BARE, JOIST_R_SEND | JOIST_R_RECV) != JOIST_OK ||
        joist_thread_create(&k, scs, &srv) != JOIST_OK ||
        joist_thread_create(&k, ccs, &cli) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
    }
}

static struct joist_msg with_cap(uint64_t v, joist_slot s, joist_rights mask)
{
    struct joist_msg m = {0};
    m.r[0] = v;
    m.ncaps = 1;
    m.cap[0] = s;
    m.mask[0] = mask;
    return m;
}

static uint32_t refs_of(joist_cspace cs, joist_slot s)
{
    struct joist_cap c;
    return joist_cap_read(&k, cs, s, &c) == JOIST_OK ? k.obj[c.obj - 1u].refs : 0;
}

JT_TEST(xfer_call_to_waiting_receiver)
{
    setup();
    const joist_slot rs[] = {10};
    JT_REQUIRE(joist_recv_slots(&k, srv, rs, 1) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    struct joist_msg m = with_cap(5, NOTE, JOIST_R_SEND | JOIST_R_GRANT);
    JT_CHECK(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[srv].msg.r[0] == 5 && k.th[srv].msg.ncaps == 1 && k.th[srv].msg.cap[0] == 10);
    struct joist_cap c;
    JT_REQUIRE(joist_cap_read(&k, scs, 10, &c) == JOIST_OK);
    JT_CHECK(c.type == JOIST_OBJ_NOTIFICATION && c.rights == (JOIST_R_SEND | JOIST_R_GRANT));
    JT_CHECK(refs_of(ccs, NOTE) == 3); /* NOTE, BARE and the server's copy */
    JT_CHECK(clean());
}

JT_TEST(xfer_call_queued_then_received)
{
    setup();
    struct joist_msg m = with_cap(6, NOTE, JOIST_R_ALL);
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(refs_of(ccs, NOTE) == 2); /* nothing copied while queued */
    const joist_slot rs[] = {11};
    JT_REQUIRE(joist_recv_slots(&k, srv, rs, 1) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[srv].msg.ncaps == 1 && k.th[srv].msg.cap[0] == 11);
    JT_CHECK(refs_of(ccs, NOTE) == 3);
    JT_CHECK(clean());
}

JT_TEST(xfer_reply_carries_capability_back)
{
    setup();
    const joist_slot crs[] = {20};
    JT_REQUIRE(joist_recv_slots(&k, cli, crs, 1) == JOIST_OK);
    struct joist_msg m = {0};
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, scs, JOIST_OBJ_NOTIFICATION, 5) == JOIST_OK);
    struct joist_msg r = with_cap(9, 5, JOIST_R_SEND);
    JT_CHECK(joist_reply(&k, srv, &r) == JOIST_OK);
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.th[cli].result == JOIST_OK);
    JT_CHECK(k.th[cli].msg.r[0] == 9 && k.th[cli].msg.cap[0] == 20);
    struct joist_cap c;
    JT_REQUIRE(joist_cap_read(&k, ccs, 20, &c) == JOIST_OK);
    JT_CHECK(c.rights == JOIST_R_SEND); /* masked */
    JT_CHECK(clean());
}

JT_TEST(xfer_bad_capability_fails_send_and_changes_nothing)
{
    setup();
    struct joist_msg m = with_cap(1, BARE, JOIST_R_ALL);
    JT_CHECK(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_E_RIGHTS); /* no GRANT */
    m.cap[0] = 30;
    JT_CHECK(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_E_EMPTY);
    m.cap[0] = JOIST_CNODE_LEAF; /* in absent leaf 1 */
    JT_CHECK(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_E_RANGE);
    m.cap[0] = NOTE;
    m.ncaps = JOIST_MSG_CAPS + 1u;
    JT_CHECK(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_E_RANGE);
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.obj[0].qkind == JOIST_Q_NONE);
    JT_CHECK(refs_of(ccs, NOTE) == 2);
    /* A bad reply keeps the reply capability; a good one then works. */
    struct joist_msg z = {0};
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &z, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    struct joist_msg bad = with_cap(2, 40, JOIST_R_ALL);
    JT_CHECK(joist_reply(&k, srv, &bad) == JOIST_E_EMPTY);
    JT_CHECK(k.th[srv].reply_to == cli + 1u && k.th[cli].state == JOIST_T_BLOCKED_REPLY);
    JT_CHECK(joist_reply_recv(&k, srv, &bad, EP_RECV, 0) == JOIST_E_EMPTY);
    JT_CHECK(k.th[srv].reply_to == cli + 1u);
    JT_CHECK(joist_reply(&k, srv, &z) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(xfer_undeliverable_capabilities_are_skipped)
{
    setup();
    JT_REQUIRE(joist_obj_create(&k, ccs, JOIST_OBJ_NOTIFICATION, 3) == JOIST_OK);
    /* Two capabilities; the server offers two slots, the first taken. */
    const joist_slot rs[] = {EP, 12};
    JT_REQUIRE(joist_recv_slots(&k, srv, rs, 2) == JOIST_OK);
    struct joist_msg m = with_cap(1, NOTE, JOIST_R_ALL);
    m.ncaps = 2;
    m.cap[1] = 3;
    m.mask[1] = JOIST_R_ALL;
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[srv].msg.ncaps == 2);
    JT_CHECK(k.th[srv].msg.cap[0] == JOIST_SLOT_NONE); /* slot EP was occupied */
    JT_CHECK(k.th[srv].msg.cap[1] == 12);              /* positional: the second still lands */
    JT_CHECK(refs_of(ccs, NOTE) == 2 && refs_of(ccs, 3) == 2);
    struct joist_msg z = {0};
    JT_REQUIRE(joist_reply(&k, srv, &z) == JOIST_OK);
    /* No slots offered: the message arrives, its capabilities don't. */
    JT_REQUIRE(joist_recv_slots(&k, srv, NULL, 0) == JOIST_OK);
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[srv].msg.ncaps == 2 && k.th[srv].msg.cap[0] == JOIST_SLOT_NONE &&
             k.th[srv].msg.cap[1] == JOIST_SLOT_NONE);
    JT_CHECK(refs_of(ccs, NOTE) == 2);
    JT_CHECK(clean());
}

JT_TEST(xfer_capability_gone_by_delivery_is_skipped)
{
    setup();
    struct joist_msg m = with_cap(1, NOTE, JOIST_R_ALL);
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    /* Another thread of the client's task deletes it while the call waits. */
    JT_REQUIRE(joist_cap_delete(&k, ccs, NOTE) == JOIST_OK);
    const joist_slot rs[] = {13};
    JT_REQUIRE(joist_recv_slots(&k, srv, rs, 1) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[srv].msg.ncaps == 1 && k.th[srv].msg.cap[0] == JOIST_SLOT_NONE);
    struct joist_cap c;
    JT_CHECK(joist_cap_read(&k, scs, 13, &c) == JOIST_E_EMPTY);
    JT_CHECK(clean());
}

JT_TEST(xfer_badged_send_capability_and_no_senders)
{
    /* A client passes the server's own badged send capability back to it
     * (as the Hurd passes ports around): the server's copy counts as a
     * sender, so no-senders fires only when every copy is gone (I4). */
    setup();
    JT_REQUIRE(joist_obj_create(&k, scs, JOIST_OBJ_NOTIFICATION, 4) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, scs, 4, 5, JOIST_R_SEND, 1) == JOIST_OK);
    JT_REQUIRE(joist_bind_nosenders(&k, scs, EP, 5) == JOIST_OK);
    JT_REQUIRE(joist_cap_mint(&k, scs, EP, 6, JOIST_R_SEND | JOIST_R_GRANT, 42) == JOIST_OK);
    JT_REQUIRE(joist__cap_grant(&k, scs, 6, ccs, 7, JOIST_R_ALL) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, scs, 6) == JOIST_OK); /* only the client's copy is left */
    const joist_slot rs[] = {14};
    JT_REQUIRE(joist_recv_slots(&k, srv, rs, 1) == JOIST_OK);
    struct joist_msg m = with_cap(1, 7, JOIST_R_SEND);
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_REQUIRE(k.th[srv].msg.cap[0] == 14);
    struct joist_cap c;
    JT_REQUIRE(joist_cap_read(&k, scs, 14, &c) == JOIST_OK);
    JT_CHECK(c.badge == 42 && c.rights == JOIST_R_SEND);
    joist_badge b;
    JT_REQUIRE(joist_cap_delete(&k, ccs, 7) == JOIST_OK);
    JT_CHECK(joist_nosenders_next(&k, scs, EP, &b) == JOIST_OK && b == 0); /* one left */
    JT_REQUIRE(joist_cap_delete(&k, scs, 14) == JOIST_OK);
    JT_CHECK(joist_nosenders_next(&k, scs, EP, &b) == JOIST_OK && b == 42);
    struct joist_msg z = {0};
    JT_CHECK(joist_reply(&k, srv, &z) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(xfer_into_leaves)
{
    setup();
    const joist_slot rs[] = {JOIST_CNODE_LEAF + 3u}; /* leaf 1: absent at first */
    JT_REQUIRE(joist_recv_slots(&k, srv, rs, 1) == JOIST_OK);
    struct joist_msg m = with_cap(1, NOTE, JOIST_R_ALL);
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[srv].msg.cap[0] == JOIST_SLOT_NONE); /* absent: no allocation on transfer */
    JT_CHECK(k.cs[scs].leaf[1] == 0);
    struct joist_msg z = {0};
    JT_REQUIRE(joist_reply(&k, srv, &z) == JOIST_OK);
    JT_REQUIRE(joist_cspace_grow(&k, scs, 1) == JOIST_OK);
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[srv].msg.cap[0] == JOIST_CNODE_LEAF + 3u);
    JT_CHECK(joist_cspace_shrink(&k, scs, 1) == JOIST_E_STATE);
    JT_CHECK(clean());
}

JT_TEST(xfer_reply_saved_carries_capability)
{
    setup();
    JT_REQUIRE(joist_obj_create(&k, scs, JOIST_OBJ_REPLY, 3) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, scs, JOIST_OBJ_NOTIFICATION, 4) == JOIST_OK);
    const joist_slot crs[] = {21};
    JT_REQUIRE(joist_recv_slots(&k, cli, crs, 1) == JOIST_OK);
    struct joist_msg z = {0};
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &z, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_REQUIRE(joist_reply_save(&k, srv, 3) == JOIST_OK);
    struct joist_msg bad = with_cap(1, 40, JOIST_R_ALL);
    JT_CHECK(joist_reply_saved(&k, srv, 3, &bad) == JOIST_E_EMPTY);
    JT_CHECK(k.obj[k.cs[scs].slot[3].obj - 1u].target == cli + 1u); /* still bound */
    struct joist_msg r = with_cap(2, 4, JOIST_R_SEND);
    JT_CHECK(joist_reply_saved(&k, srv, 3, &r) == JOIST_OK);
    JT_CHECK(k.th[cli].msg.cap[0] == 21 && k.th[cli].msg.r[0] == 2);
    JT_CHECK(clean());
}

JT_TEST(xfer_recv_slots_errors)
{
    setup();
    const joist_slot rs[] = {1, 2, 3};
    JT_CHECK(joist_recv_slots(&k, srv, rs, JOIST_MSG_CAPS + 1u) == JOIST_E_RANGE);
    JT_CHECK(joist_recv_slots(&k, JOIST_MAX_THREADS, rs, 1) == JOIST_E_RANGE);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_recv_slots(&k, srv, rs, 1) == JOIST_E_STATE);
    JT_CHECK(k.th[srv].nrecv == 0);
    JT_CHECK(clean());
}

JT_TEST(xfer_no_slots_offered_lands_nowhere)
{
    /* A receiver that offered no slots gets nothing, even where its slot 0
     * is free (unused positions must not default to slot 0). */
    setup();
    joist_cspace fresh;
    joist_thread t2;
    JT_REQUIRE(joist_cspace_create(&k, 4, &fresh) == JOIST_OK);
    JT_REQUIRE(joist_thread_create(&k, fresh, &t2) == JOIST_OK);
    JT_REQUIRE(joist__cap_grant(&k, scs, EP, fresh, 5, JOIST_R_RECV) == JOIST_OK);
    struct joist_msg m = with_cap(1, NOTE, JOIST_R_ALL);
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, t2, 5, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[t2].msg.ncaps == 1 && k.th[t2].msg.cap[0] == JOIST_SLOT_NONE);
    struct joist_cap c;
    JT_CHECK(joist_cap_read(&k, fresh, 0, &c) == JOIST_E_EMPTY);
    struct joist_msg z = {0};
    JT_CHECK(joist_reply(&k, t2, &z) == JOIST_OK);
    JT_CHECK(clean());
}
