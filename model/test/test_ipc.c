/* test_ipc.c - unit tests for threads, endpoints and synchronous IPC (Phase 1
 * work item 2; invariants I6, I7, I10). */
#include "harness.h"
#include "joist/model.h"

static struct joist_kernel k;
static joist_cspace srv_cs, cli_cs;
static joist_thread srv, cli;

enum { EP = 0, EP_SEND = 1, EP_RECV = 2, NOTE = 3 };

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

static struct joist_msg mk(uint64_t v)
{
    struct joist_msg m = {{0}};
    m.r[0] = v;
    return m;
}

/* One space holds the endpoint with all rights plus send-only and
 * receive-only derivations. Client and server threads share it; the model
 * does not need capability transfer to test IPC. */
static void setup(void)
{
    joist_init(&k);
    if (joist_cspace_create(&k, 8, &srv_cs) != JOIST_OK ||
        joist_obj_create(&k, srv_cs, JOIST_OBJ_ENDPOINT, EP) != JOIST_OK ||
        joist_cap_mint(&k, srv_cs, EP, EP_SEND, JOIST_R_SEND, 77) != JOIST_OK ||
        joist_cap_derive(&k, srv_cs, EP, EP_RECV, JOIST_R_RECV) != JOIST_OK ||
        joist_obj_create(&k, srv_cs, JOIST_OBJ_NOTIFICATION, NOTE) != JOIST_OK ||
        joist_thread_create(&k, srv_cs, &srv) != JOIST_OK ||
        joist_thread_create(&k, srv_cs, &cli) != JOIST_OK) {
        fprintf(stderr, "  setup failed\n");
    }
    cli_cs = srv_cs;
}

JT_TEST(ipc_send_then_recv)
{
    setup();
    struct joist_msg m = mk(5);
    JT_CHECK(joist_send(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[cli].state == JOIST_T_BLOCKED_SEND);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[srv].msg.r[0] == 5);
    JT_CHECK(k.th[srv].badge == 77); /* I7: the badge of the capability used */
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.th[cli].result == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(ipc_recv_then_send)
{
    setup();
    struct joist_msg m = mk(9);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_send(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[srv].state == JOIST_T_RUNNING && k.th[srv].result == JOIST_OK);
    JT_CHECK(k.th[srv].msg.r[0] == 9 && k.th[srv].badge == 77);
    JT_CHECK(clean());
}

JT_TEST(ipc_call_reply_round_trip)
{
    setup();
    struct joist_msg q = mk(1), a = mk(2);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_call(&k, cli, EP_SEND, &q, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[cli].state == JOIST_T_BLOCKED_REPLY);
    JT_CHECK(k.th[srv].msg.r[0] == 1);
    JT_CHECK(clean());
    JT_CHECK(joist_reply(&k, srv, &a) == JOIST_OK);
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.th[cli].result == JOIST_OK);
    JT_CHECK(k.th[cli].msg.r[0] == 2);
    /* I6: the reply capability is single-use. */
    JT_CHECK(joist_reply(&k, srv, &a) == JOIST_E_NOREPLY);
    JT_CHECK(clean());
}

JT_TEST(ipc_call_before_server_waits)
{
    setup();
    struct joist_msg q = mk(3), a = mk(4);
    JT_CHECK(joist_call(&k, cli, EP_SEND, &q, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[cli].state == JOIST_T_BLOCKED_SEND);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[cli].state == JOIST_T_BLOCKED_REPLY);
    JT_CHECK(joist_reply(&k, srv, &a) == JOIST_OK);
    JT_CHECK(k.th[cli].result == JOIST_OK && k.th[cli].msg.r[0] == 4);
    JT_CHECK(clean());
}

JT_TEST(ipc_rights_and_types)
{
    setup();
    struct joist_msg m = mk(0);
    JT_CHECK(joist_send(&k, cli, EP_RECV, &m, JOIST_FOREVER) == JOIST_E_RIGHTS);
    JT_CHECK(joist_recv(&k, srv, EP_SEND, JOIST_FOREVER) == JOIST_E_RIGHTS);
    JT_CHECK(joist_send(&k, cli, NOTE, &m, JOIST_FOREVER) == JOIST_E_TYPE);
    JT_CHECK(joist_send(&k, cli, 40, &m, JOIST_FOREVER) == JOIST_E_EMPTY);
    JT_CHECK(joist_send(&k, cli, JOIST_CSPACE_SLOTS, &m, JOIST_FOREVER) == JOIST_E_RANGE);
    JT_CHECK(joist_send(&k, JOIST_MAX_THREADS, EP_SEND, &m, JOIST_FOREVER) == JOIST_E_RANGE);
    JT_CHECK(clean());
}

JT_TEST(ipc_timeout_zero_never_blocks)
{
    setup();
    struct joist_msg m = mk(0);
    JT_CHECK(joist_send(&k, cli, EP_SEND, &m, 0) == JOIST_E_TIMEOUT);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, 0) == JOIST_E_TIMEOUT);
    /* A Call always waits for a reply, so timeout 0 is refused up front. */
    JT_CHECK(joist_call(&k, cli, EP_SEND, &m, 0) == JOIST_E_TIMEOUT);
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.th[srv].state == JOIST_T_RUNNING);
    JT_CHECK(k.obj[0].qkind == JOIST_Q_NONE);
    JT_CHECK(clean());
}

JT_TEST(ipc_timeout_expires_exactly_at_deadline)
{
    setup();
    struct joist_msg m = mk(0);
    JT_CHECK(joist_send(&k, cli, EP_SEND, &m, 10) == JOIST_BLOCKED);
    joist_tick(&k, 9);
    JT_CHECK(k.th[cli].state == JOIST_T_BLOCKED_SEND); /* not yet */
    joist_tick(&k, 1);
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.th[cli].result == JOIST_E_TIMEOUT);
    JT_CHECK(k.obj[0].qkind == JOIST_Q_NONE); /* dequeued */
    JT_CHECK(clean());
}

JT_TEST(ipc_caller_timeout_makes_reply_stale)
{
    setup();
    struct joist_msg q = mk(1), a = mk(2);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_call(&k, cli, EP_SEND, &q, 5) == JOIST_BLOCKED);
    joist_tick(&k, 5);
    JT_CHECK(k.th[cli].result == JOIST_E_TIMEOUT);
    JT_CHECK(clean()); /* a stale reply capability is allowed; a waiting caller is not */
    /* The caller stopped waiting: Reply says so, and consumes the capability. */
    JT_CHECK(joist_reply(&k, srv, &a) == JOIST_E_PEER);
    JT_CHECK(k.th[srv].reply_to == 0);
    JT_CHECK(k.th[cli].msg.r[0] != 2);             /* the late reply was not delivered */
    JT_CHECK(k.th[cli].result == JOIST_E_TIMEOUT); /* and its outcome is unchanged */
    JT_CHECK(clean());
}

JT_TEST(ipc_endpoint_destroyed_wakes_waiters)
{
    setup();
    joist_thread t2;
    JT_REQUIRE(joist_thread_create(&k, srv_cs, &t2) == JOIST_OK);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_recv(&k, t2, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_cap_delete(&k, srv_cs, EP) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, srv_cs, EP_SEND) == JOIST_OK);
    JT_CHECK(k.th[srv].state == JOIST_T_BLOCKED_RECV); /* one capability left */
    JT_CHECK(joist_cap_delete(&k, srv_cs, EP_RECV) == JOIST_OK);
    /* I10: the endpoint is gone and both waiters woke with E_GONE. */
    JT_CHECK(k.th[srv].state == JOIST_T_RUNNING && k.th[srv].result == JOIST_E_GONE);
    JT_CHECK(k.th[t2].state == JOIST_T_RUNNING && k.th[t2].result == JOIST_E_GONE);
    JT_CHECK(clean());
}

JT_TEST(ipc_server_death_wakes_caller)
{
    setup();
    struct joist_msg q = mk(1);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_call(&k, cli, EP_SEND, &q, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_thread_destroy(&k, srv) == JOIST_OK);
    /* I6/I10: no-timeout caller is not left waiting forever. */
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.th[cli].result == JOIST_E_PEER);
    JT_CHECK(clean());
}

JT_TEST(ipc_recv_refused_while_holding_reply)
{
    setup();
    struct joist_msg q = mk(1), a = mk(2);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_call(&k, cli, EP_SEND, &q, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_E_STATE);
    JT_CHECK(joist_reply_recv(&k, srv, &a, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(k.th[cli].result == JOIST_OK && k.th[cli].msg.r[0] == 2);
    JT_CHECK(k.th[srv].state == JOIST_T_BLOCKED_RECV);
    JT_CHECK(clean());
}

JT_TEST(ipc_senders_are_fifo)
{
    setup();
    joist_thread a, b;
    struct joist_msg ma = mk(10), mb = mk(20);
    JT_REQUIRE(joist_thread_create(&k, cli_cs, &a) == JOIST_OK);
    JT_REQUIRE(joist_thread_create(&k, cli_cs, &b) == JOIST_OK);
    JT_CHECK(joist_send(&k, a, EP_SEND, &ma, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_send(&k, b, EP_SEND, &mb, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, 0) == JOIST_OK && k.th[srv].msg.r[0] == 10);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, 0) == JOIST_OK && k.th[srv].msg.r[0] == 20);
    JT_CHECK(clean());
}

JT_TEST(ipc_stale_reply_cannot_reach_a_reused_thread)
{
    setup();
    struct joist_msg q = mk(1), a = mk(99);
    joist_thread c2;
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_call(&k, cli, EP_SEND, &q, JOIST_FOREVER) == JOIST_BLOCKED);
    /* The caller dies; a new thread takes its slot and makes its own call to
     * a second server thread. The first server's stale reply must not reach it. */
    JT_CHECK(joist_thread_destroy(&k, cli) == JOIST_OK);
    JT_REQUIRE(joist_thread_create(&k, cli_cs, &c2) == JOIST_OK);
    JT_CHECK(c2 == cli); /* same slot reused */
    joist_thread srv2;
    JT_REQUIRE(joist_thread_create(&k, srv_cs, &srv2) == JOIST_OK);
    JT_CHECK(joist_recv(&k, srv2, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_call(&k, c2, EP_SEND, &q, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_reply(&k, srv, &a) == JOIST_E_PEER);
    JT_CHECK(k.th[c2].state == JOIST_T_BLOCKED_REPLY); /* still waiting for srv2 */
    JT_CHECK(clean());
}

JT_TEST(ipc_destroying_a_space_destroys_its_threads)
{
    setup();
    joist_cspace other;
    joist_thread caller;
    struct joist_msg q = mk(1);
    JT_REQUIRE(joist_cspace_create(&k, 1, &other) == JOIST_OK);
    JT_REQUIRE(joist_thread_create(&k, other, &caller) == JOIST_OK);
    /* The caller lives in another space but calls through srv_cs's endpoint
     * via a thread there; destroy the server's space under it. */
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_call(&k, cli, EP_SEND, &q, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_cspace_destroy(&k, srv_cs) == JOIST_OK);
    JT_CHECK(k.th[srv].state == JOIST_T_FREE && k.th[cli].state == JOIST_T_FREE);
    JT_CHECK(k.objects_live == 0);
    JT_CHECK(k.th[caller].state == JOIST_T_RUNNING);
    JT_CHECK(clean());
}

JT_TEST(ipc_blocked_thread_cannot_act)
{
    setup();
    struct joist_msg m = mk(0);
    JT_CHECK(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_send(&k, srv, EP_SEND, &m, 0) == JOIST_E_STATE);
    JT_CHECK(joist_reply(&k, srv, &m) == JOIST_E_STATE);
    JT_CHECK(clean());
}

/* ---- deferred replies (work item 6: what proc_wait, select and terminal
 * reads need, and what makes a bare Send unnecessary) ---- */

enum { RPL1 = 10, RPL2 = 11 };

static joist_thread cli2;

static void setup_deferred(void)
{
    setup();
    if (joist_obj_create(&k, srv_cs, JOIST_OBJ_REPLY, RPL1) != JOIST_OK ||
        joist_obj_create(&k, srv_cs, JOIST_OBJ_REPLY, RPL2) != JOIST_OK ||
        joist_thread_create(&k, srv_cs, &cli2) != JOIST_OK) {
        fprintf(stderr, "  setup_deferred failed\n");
    }
}

JT_TEST(ipc_deferred_replies_out_of_order)
{
    setup_deferred();
    struct joist_msg m = mk(1);
    struct joist_msg r1 = mk(101), r2 = mk(102);
    /* Two calls arrive; the server parks each and keeps receiving. */
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, cli2, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(joist_reply_save(&k, srv, RPL1) == JOIST_OK);
    JT_CHECK(k.th[srv].reply_to == 0); /* moved, not copied */
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(joist_reply_save(&k, srv, RPL2) == JOIST_OK);
    JT_CHECK(clean());
    /* Answered in the other order, each reaching its own caller. */
    JT_CHECK(joist_reply_saved(&k, srv, RPL2, &r2) == JOIST_OK);
    JT_CHECK(k.th[cli2].state == JOIST_T_RUNNING && k.th[cli2].msg.r[0] == 102);
    JT_CHECK(k.th[cli].state == JOIST_T_BLOCKED_REPLY);
    JT_CHECK(joist_reply_saved(&k, srv, RPL1, &r1) == JOIST_OK);
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.th[cli].msg.r[0] == 101);
    /* I6: single-use, and the object is free for the next call. */
    JT_CHECK(joist_reply_saved(&k, srv, RPL1, &r1) == JOIST_E_NOREPLY);
    JT_CHECK(clean());
}

JT_TEST(ipc_deferred_reply_from_another_thread)
{
    setup_deferred();
    struct joist_msg m = mk(1);
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_REQUIRE(joist_reply_save(&k, srv, RPL1) == JOIST_OK);
    /* A worker thread in the same space answers; the receiver dying doesn't
     * matter, since the reply now belongs to the space. */
    JT_CHECK(joist_thread_destroy(&k, srv) == JOIST_OK);
    JT_CHECK(k.th[cli].state == JOIST_T_BLOCKED_REPLY);
    JT_CHECK(joist_reply_saved(&k, cli2, RPL1, &m) == JOIST_OK);
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.th[cli].result == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(ipc_deferred_reply_object_deleted_wakes_caller)
{
    setup_deferred();
    struct joist_msg m = mk(1);
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_REQUIRE(joist_reply_save(&k, srv, RPL1) == JOIST_OK);
    JT_CHECK(joist_cap_delete(&k, srv_cs, RPL1) == JOIST_OK);
    /* I10: nobody can answer now, so the caller doesn't wait forever. */
    JT_CHECK(k.th[cli].state == JOIST_T_RUNNING && k.th[cli].result == JOIST_E_PEER);
    JT_CHECK(clean());
}

JT_TEST(ipc_deferred_reply_to_departed_caller)
{
    setup_deferred();
    struct joist_msg m = mk(1);
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, 5) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_REQUIRE(joist_reply_save(&k, srv, RPL1) == JOIST_OK);
    joist_tick(&k, 5); /* the call's timeout covers the whole call */
    JT_CHECK(k.th[cli].result == JOIST_E_TIMEOUT);
    JT_CHECK(joist_reply_saved(&k, srv, RPL1, &m) == JOIST_E_PEER);
    JT_CHECK(k.obj[k.cs[srv_cs].slot[RPL1].obj - 1u].target == 0); /* consumed anyway */
    JT_CHECK(clean());
}

JT_TEST(ipc_deferred_reply_errors)
{
    setup_deferred();
    struct joist_msg m = mk(1);
    JT_CHECK(joist_reply_save(&k, srv, RPL1) == JOIST_E_NOREPLY);
    JT_CHECK(joist_reply_saved(&k, srv, RPL1, &m) == JOIST_E_NOREPLY);
    JT_CHECK(joist_reply_save(&k, srv, EP) == JOIST_E_TYPE);
    JT_CHECK(joist_reply_save(&k, srv, 20) == JOIST_E_EMPTY);
    JT_CHECK(joist_reply_save(&k, srv, JOIST_CSPACE_SLOTS) == JOIST_E_RANGE);
    JT_CHECK(joist_reply_save(&k, JOIST_MAX_THREADS, RPL1) == JOIST_E_RANGE);
    JT_REQUIRE(joist_cap_derive(&k, srv_cs, RPL1, 12, JOIST_R_GRANT) == JOIST_OK);
    JT_CHECK(joist_reply_saved(&k, srv, 12, &m) == JOIST_E_RIGHTS);
    /* A bound object can't take a second reply capability. */
    JT_REQUIRE(joist_call(&k, cli, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, cli2, EP_SEND, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_REQUIRE(joist_reply_save(&k, srv, RPL1) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, srv, EP_RECV, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(joist_reply_save(&k, srv, RPL1) == JOIST_E_OCCUPIED);
    JT_CHECK(k.th[srv].reply_to != 0); /* unchanged: still the server's */
    JT_CHECK(clean());
}
