/* test_fault.c - unit tests for thread control and fault handling (Phase 3
 * step 3; decisions 0009, 0010). The root makes a task T with a worker w
 * and a handler thread h; T holds the fault endpoint FEP, and h receives on
 * it with receive slot HCAP for the thread capability a fault carries. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

static struct joist_kernel k;
static joist_cspace root, T;
static joist_thread w, h;

enum { R_TASK = 0, R_W = 1, R_H = 2, R_SC = 3 }; /* root's slots */
enum { FEP = 0, HCAP = 5 };                     /* T's slots */

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
    const joist_slot rs[] = {HCAP};
    if (joist_boot(&k, 64, &root) != JOIST_OK || joist_task_create(&k, root, 30, R_TASK) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    T = k.obj[k.cs[root].slot[R_TASK].obj - 1u].target - 1u;
    if (joist_thread_spawn(&k, root, R_TASK, R_W, &w) != JOIST_OK ||
        joist_thread_spawn(&k, root, R_TASK, R_H, &h) != JOIST_OK ||
        joist_obj_create(&k, T, JOIST_OBJ_ENDPOINT, FEP) != JOIST_OK ||
        joist_recv_slots(&k, h, rs, 1) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
    }
}

JT_TEST(fault_regs_read_and_write)
{
    setup();
    struct joist_regs r = {0};
    r.pc = 0x401000;
    r.sp = 0x7ff000;
    r.tls = 0x5000;
    r.r[3] = 42;
    JT_REQUIRE(joist_thread_write_regs(&k, root, R_W, &r) == JOIST_OK);
    struct joist_regs back;
    JT_REQUIRE(joist_thread_read_regs(&k, root, R_W, &back) == JOIST_OK);
    JT_CHECK(back.pc == 0x401000 && back.sp == 0x7ff000 && back.tls == 0x5000 && back.r[3] == 42);
    JT_REQUIRE(joist_cap_derive(&k, root, R_W, 6, JOIST_R_GRANT) == JOIST_OK);
    JT_CHECK(joist_thread_read_regs(&k, root, 6, &back) == JOIST_E_RIGHTS); /* no CONTROL */
    JT_CHECK(joist_thread_read_regs(&k, root, R_TASK, &back) == JOIST_E_TYPE);
    JT_CHECK(clean());
}

JT_TEST(fault_abort_cancels_blocking)
{
    setup();
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_thread_abort(&k, root, R_H) == JOIST_OK);
    JT_CHECK(k.th[h].state == JOIST_T_RUNNING && k.th[h].result == JOIST_E_ABORTED);
    JT_CHECK(k.obj[k.cs[T].slot[FEP].obj - 1u].qhead == 0); /* off the queue */
    /* A caller waiting for its reply stops; the late reply finds nobody. */
    struct joist_msg m = {0};
    JT_REQUIRE(joist_call(&k, w, FEP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_OK);
    JT_REQUIRE(joist_thread_abort(&k, root, R_W) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_RUNNING && k.th[w].result == JOIST_E_ABORTED);
    JT_CHECK(joist_reply(&k, h, &m) == JOIST_E_PEER);
    /* Aborting a running thread changes nothing. */
    JT_CHECK(joist_thread_abort(&k, root, R_W) == JOIST_OK && k.th[w].state == JOIST_T_RUNNING);
    JT_CHECK(clean());
}

JT_TEST(fault_suspend_and_resume)
{
    setup();
    JT_REQUIRE(joist_thread_suspend(&k, root, R_W) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_SUSPENDED);
    struct joist_msg m = {0};
    JT_CHECK(joist_call(&k, w, FEP, &m, JOIST_FOREVER) == JOIST_E_STATE); /* it can't act */
    JT_CHECK(joist_thread_suspend(&k, root, R_W) == JOIST_OK);             /* twice is fine */
    JT_REQUIRE(joist_thread_resume(&k, root, R_W) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_RUNNING);
    JT_CHECK(joist_thread_resume(&k, root, R_W) == JOIST_E_STATE);
    /* Suspending a blocked thread takes it off its queue first. */
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_thread_suspend(&k, root, R_H) == JOIST_OK);
    JT_CHECK(k.th[h].state == JOIST_T_SUSPENDED && k.th[h].result == JOIST_E_ABORTED);
    JT_CHECK(k.obj[k.cs[T].slot[FEP].obj - 1u].qhead == 0);
    JT_CHECK(clean());
}

JT_TEST(fault_goes_to_thread_handler_and_resumes)
{
    setup();
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_fault(&k, w, JOIST_FAULT_PAGE, 0xdead000, 6) == JOIST_BLOCKED);
    JT_CHECK(k.th[w].faulting && k.th[w].state == JOIST_T_BLOCKED_REPLY);
    JT_CHECK(k.th[h].msg.r[0] == JOIST_FAULT_PAGE && k.th[h].msg.r[1] == 0xdead000 &&
             k.th[h].msg.r[2] == 6);
    /* The handler got a control capability for w, and uses it. */
    JT_CHECK(k.th[h].msg.ncaps == 1 && k.th[h].msg.cap[0] == HCAP);
    struct joist_regs r;
    JT_REQUIRE(joist_thread_read_regs(&k, T, HCAP, &r) == JOIST_OK);
    r.pc = 0x402000;
    JT_REQUIRE(joist_thread_write_regs(&k, T, HCAP, &r) == JOIST_OK);
    struct joist_msg resume = {0};
    resume.r[0] = JOIST_FAULT_RESUME;
    JT_CHECK(joist_reply(&k, h, &resume) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_RUNNING && !k.th[w].faulting && k.th[w].regs.pc == 0x402000);
    JT_CHECK(clean());
}

JT_TEST(fault_handler_terminates_thread)
{
    setup();
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_UD, 0, 0) == JOIST_BLOCKED);
    struct joist_msg kill = {0};
    kill.r[0] = 1;
    JT_CHECK(joist_reply(&k, h, &kill) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_FREE);
    struct joist_regs r;
    JT_CHECK(joist_thread_read_regs(&k, root, R_W, &r) == JOIST_E_GONE);
    JT_CHECK(clean());
}

JT_TEST(fault_without_handler_terminates_task)
{
    setup();
    JT_CHECK(joist_fault(&k, w, JOIST_FAULT_GP, 0, 0) == JOIST_E_PEER);
    JT_CHECK(!k.cs[T].live && k.th[w].state == JOIST_T_FREE && k.th[h].state == JOIST_T_FREE);
    JT_CHECK(clean());
}

JT_TEST(fault_falls_back_to_task_handler)
{
    setup();
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP) == JOIST_OK);
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, 9) == JOIST_OK); /* names an empty slot */
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_fault(&k, w, JOIST_FAULT_MATH, 0, 0) == JOIST_BLOCKED);
    JT_CHECK(k.th[h].msg.r[0] == JOIST_FAULT_MATH && k.th[w].faulting);
    JT_CHECK(clean());
}

JT_TEST(fault_unresolved_leaves_thread_suspended)
{
    setup();
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x10, 4) == JOIST_BLOCKED);
    JT_REQUIRE(joist_thread_kill(&k, root, R_H) == JOIST_OK); /* the handler dies */
    JT_CHECK(k.th[w].state == JOIST_T_SUSPENDED && !k.th[w].faulting &&
             k.th[w].result == JOIST_E_PEER);
    JT_REQUIRE(joist_thread_resume(&k, root, R_W) == JOIST_OK);
    /* Aborting a fault call also leaves the thread stopped. */
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x10, 4) == JOIST_BLOCKED); /* nobody receives */
    JT_CHECK(k.th[w].state == JOIST_T_BLOCKED_SEND);
    JT_REQUIRE(joist_thread_abort(&k, root, R_W) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_SUSPENDED && k.th[w].result == JOIST_E_ABORTED);
    JT_CHECK(clean());
}

JT_TEST(fault_queued_then_received_carries_capability)
{
    setup();
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP) == JOIST_OK);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x20, 0) == JOIST_BLOCKED);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK(k.th[h].msg.r[1] == 0x20 && k.th[h].msg.cap[0] == HCAP);
    struct joist_cap c;
    JT_REQUIRE(joist_cap_read(&k, T, HCAP, &c) == JOIST_OK);
    JT_CHECK(c.type == JOIST_OBJ_THREAD && c.rights == JOIST_R_CONTROL);
    JT_CHECK(clean());
}

JT_TEST(fault_is_charged_to_the_faulting_thread)
{
    setup();
    JT_REQUIRE(joist_sc_create(&k, root, R_SC, 9, 2, 4) == JOIST_OK);
    JT_REQUIRE(joist_sc_bind(&k, root, R_SC, R_W) == JOIST_OK);
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0, 0) == JOIST_BLOCKED);
    JT_CHECK(k.th[h].run_sc == k.cs[root].slot[R_SC].obj && k.th[w].run_sc == 0);
    struct joist_msg resume = {0};
    JT_REQUIRE(joist_reply(&k, h, &resume) == JOIST_OK);
    JT_CHECK(k.th[w].run_sc == k.cs[root].slot[R_SC].obj && k.th[h].run_sc == 0);
    JT_CHECK(clean());
}

JT_TEST(fault_dead_handler_endpoint_is_no_handler)
{
    /* The thread names an endpoint whose server is gone: no handler, the
     * task is terminated. */
    setup();
    JT_REQUIRE(joist_obj_create(&k, T, JOIST_OBJ_ENDPOINT, 7) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, T, 7, 8, JOIST_R_SEND) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, T, 7) == JOIST_OK); /* the last RECV: dead */
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, 8) == JOIST_OK);
    JT_CHECK(joist_fault(&k, w, JOIST_FAULT_PAGE, 0, 0) == JOIST_E_PEER);
    JT_CHECK(!k.cs[T].live);
    JT_CHECK(clean());
}

JT_TEST(fault_in_root_task_ends_only_the_thread)
{
    setup();
    joist_thread rt;
    JT_REQUIRE(joist_thread_create(&k, root, &rt) == JOIST_OK);
    JT_CHECK(joist_fault(&k, rt, JOIST_FAULT_GP, 0, 0) == JOIST_E_PEER);
    JT_CHECK(k.cs[root].live && k.th[rt].state == JOIST_T_FREE);
    JT_CHECK(joist_fault(&k, rt, JOIST_FAULT_GP, 0, 0) == JOIST_E_STATE);
    JT_CHECK(clean());
}
