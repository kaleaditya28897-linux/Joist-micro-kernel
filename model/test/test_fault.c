/* SPDX-License-Identifier: GPL-3.0-only */
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
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP, 0) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_fault(&k, w, JOIST_FAULT_PAGE, 0xdead000, 6, 0) == JOIST_BLOCKED);
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

JT_TEST(fault_message_carries_the_pc_and_the_access)
{
    /* Phase 4 (0007): the faulting pc in r[3]; a page fault's access, read,
     * write or execute and whether the page was present, in r[0] beside
     * the kind, independent of the architecture's code (r[2]). */
    setup();
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP, 0) == JOIST_OK);
    k.th[w].regs.pc = 0x401234;
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x5000, 7,
                           JOIST_ACCESS_WRITE | JOIST_ACCESS_PRESENT) == JOIST_BLOCKED);
    const struct joist_msg *m = &k.th[h].msg;
    JT_CHECK(JOIST_FAULT_KIND(m->r[0]) == JOIST_FAULT_PAGE &&
             JOIST_FAULT_ACCESS(m->r[0]) == (JOIST_ACCESS_WRITE | JOIST_ACCESS_PRESENT));
    JT_CHECK(m->r[1] == 0x5000 && m->r[2] == 7 && m->r[3] == 0x401234);
    struct joist_msg resume = {0};
    JT_REQUIRE(joist_reply(&k, h, &resume) == JOIST_OK);
    /* Another kind carries no access, whatever is passed. */
    JT_REQUIRE(joist_cap_delete(&k, T, HCAP) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_UD, 0, 0, JOIST_ACCESS_EXEC) == JOIST_BLOCKED);
    JT_CHECK(k.th[h].msg.r[0] == JOIST_FAULT_UD && k.th[h].msg.r[3] == 0x401234);
    JT_REQUIRE(joist_reply(&k, h, &resume) == JOIST_OK);
    JT_CHECK(clean());
}

JT_TEST(fault_no_thread_capability_on_page_faults_if_asked)
{
    /* JOIST_FAULT_NO_THREAD_CAP: a page fault brings no capability (the
     * badge names the task) and the reply still resumes the thread; other
     * faults still bring one. The task's handler's flag holds for threads
     * without their own handler. */
    setup();
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP, JOIST_FAULT_NO_THREAD_CAP) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x6000, 4, JOIST_ACCESS_READ) == JOIST_BLOCKED);
    JT_CHECK(k.th[h].msg.ncaps == 0 && k.cs[T].slot[HCAP].obj == 0 && k.th[w].faulting == 2u);
    struct joist_msg resume = {0};
    JT_REQUIRE(joist_reply(&k, h, &resume) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_RUNNING && !k.th[w].faulting);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_GP, 0, 0, 0) == JOIST_BLOCKED);
    JT_CHECK(k.th[h].msg.ncaps == 1 && k.th[h].msg.cap[0] == HCAP && k.th[w].faulting == 1u);
    JT_REQUIRE(joist_reply(&k, h, &resume) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, T, HCAP) == JOIST_OK);
    /* Through the task's handler. */
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, JOIST_SLOT_NONE, 0) == JOIST_OK);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, JOIST_FAULT_NO_THREAD_CAP) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x6000, 4, JOIST_ACCESS_READ) == JOIST_BLOCKED);
    JT_CHECK(k.th[h].msg.ncaps == 0 && k.cs[T].slot[HCAP].obj == 0);
    JT_REQUIRE(joist_reply(&k, h, &resume) == JOIST_OK);
    /* An unknown flag: refused, nothing set. */
    JT_CHECK(joist_thread_set_fault(&k, root, R_W, FEP, 0x2u) == JOIST_E_RANGE);
    JT_CHECK(joist_task_set_fault(&k, root, R_TASK, FEP, 0x80u) == JOIST_E_RANGE);
    JT_CHECK(k.th[w].fault_slot == 0 && k.cs[T].fault_flags == JOIST_FAULT_NO_THREAD_CAP);
    JT_CHECK(clean());
}

JT_TEST(fault_handler_terminates_thread)
{
    setup();
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP, 0) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_UD, 0, 0, 0) == JOIST_BLOCKED);
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
    JT_CHECK(joist_fault(&k, w, JOIST_FAULT_GP, 0, 0, 0) == JOIST_E_PEER);
    JT_CHECK(!k.cs[T].live && k.th[w].state == JOIST_T_FREE && k.th[h].state == JOIST_T_FREE);
    JT_CHECK(clean());
}

JT_TEST(fault_falls_back_to_task_handler)
{
    setup();
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, 0) == JOIST_OK);
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, 9, 0) == JOIST_OK); /* names an empty slot */
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_fault(&k, w, JOIST_FAULT_MATH, 0, 0, 0) == JOIST_BLOCKED);
    JT_CHECK(k.th[h].msg.r[0] == JOIST_FAULT_MATH && k.th[w].faulting);
    JT_CHECK(clean());
}

JT_TEST(fault_unresolved_leaves_thread_suspended)
{
    setup();
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP, 0) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x10, 4, 0) == JOIST_BLOCKED);
    JT_REQUIRE(joist_thread_kill(&k, root, R_H) == JOIST_OK); /* the handler dies */
    JT_CHECK(k.th[w].state == JOIST_T_SUSPENDED && !k.th[w].faulting &&
             k.th[w].result == JOIST_E_PEER);
    JT_REQUIRE(joist_thread_resume(&k, root, R_W) == JOIST_OK);
    /* Aborting a fault call also leaves the thread stopped. */
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x10, 4, 0) == JOIST_BLOCKED); /* nobody receives */
    JT_CHECK(k.th[w].state == JOIST_T_BLOCKED_SEND);
    JT_REQUIRE(joist_thread_abort(&k, root, R_W) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_SUSPENDED && k.th[w].result == JOIST_E_ABORTED);
    JT_CHECK(clean());
}

JT_TEST(fault_queued_then_received_carries_capability)
{
    setup();
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP, 0) == JOIST_OK);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x20, 0, 0) == JOIST_BLOCKED);
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
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP, 0) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0, 0, 0) == JOIST_BLOCKED);
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
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, 8, 0) == JOIST_OK);
    JT_CHECK(joist_fault(&k, w, JOIST_FAULT_PAGE, 0, 0, 0) == JOIST_E_PEER);
    JT_CHECK(!k.cs[T].live);
    JT_CHECK(clean());
}

JT_TEST(fault_in_root_task_ends_only_the_thread)
{
    setup();
    joist_thread rt;
    JT_REQUIRE(joist_thread_create(&k, root, &rt) == JOIST_OK);
    JT_CHECK(joist_fault(&k, rt, JOIST_FAULT_GP, 0, 0, 0) == JOIST_E_PEER);
    JT_CHECK(k.cs[root].live && k.th[rt].state == JOIST_T_FREE);
    JT_CHECK(joist_fault(&k, rt, JOIST_FAULT_GP, 0, 0, 0) == JOIST_E_STATE);
    JT_CHECK(clean());
}

/* ---- Two roles a task (0027): a pager for page faults, an exception
 * handler for the rest. The pager is a thread p of another task M (as mm
 * is), receiving on MEP; T holds a badged SEND capability to it in PS. */

static joist_cspace M;
static joist_thread p;
enum { R_M = 4, R_P = 5 };              /* root's slots */
enum { MEP = 0, MBADGED = 3, PCAP = 6 }; /* M's slots */
enum { PS = 1 };                         /* T's slot naming the pager */
#define PAGER_BADGE 0x77u

static void setup_pager(unsigned flags)
{
    setup();
    const joist_slot rs[] = {PCAP};
    if (joist_task_create(&k, root, 12, R_M) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the pager's task could not be made", 0);
        return;
    }
    M = k.obj[k.cs[root].slot[R_M].obj - 1u].target - 1u;
    if (joist_thread_spawn(&k, root, R_M, R_P, &p) != JOIST_OK ||
        joist_obj_create(&k, M, JOIST_OBJ_ENDPOINT, MEP) != JOIST_OK ||
        joist_cap_mint(&k, M, MEP, MBADGED, JOIST_R_SEND | JOIST_R_GRANT, PAGER_BADGE) != JOIST_OK ||
        joist__cap_grant(&k, M, MBADGED, T, PS, JOIST_R_SEND) != JOIST_OK ||
        joist_recv_slots(&k, p, rs, 1) != JOIST_OK ||
        joist_task_set_pager(&k, root, R_TASK, PS, flags) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the pager could not be set up as written", 0);
    }
}

static int pager_waits(void)
{
    return joist_recv(&k, p, MEP, JOIST_FOREVER) == JOIST_BLOCKED;
}

static int handler_waits(void)
{
    return joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_BLOCKED;
}

static joist_err verdict(joist_thread from, uint64_t r0, uint64_t r1)
{
    struct joist_msg m = {0};
    m.r[0] = r0;
    m.r[1] = r1;
    return joist_reply(&k, from, &m);
}

JT_TEST(pager_set_checks_like_the_handlers)
{
    setup_pager(0);
    JT_CHECK(k.cs[T].pager_slot == PS + 1u && k.cs[T].pager_flags == 0);
    /* Task level only: a thread capability is the wrong type. */
    JT_CHECK(joist_task_set_pager(&k, root, R_W, PS, 0) == JOIST_E_TYPE);
    JT_CHECK(joist_task_set_pager(&k, root, R_TASK, PS, 0x2u) == JOIST_E_RANGE);
    JT_CHECK(joist_task_set_pager(&k, root, R_TASK, JOIST_CSPACE_SLOTS, 0) == JOIST_E_RANGE);
    JT_CHECK(k.cs[T].pager_slot == PS + 1u); /* refusals change nothing */
    JT_REQUIRE(joist_cap_derive(&k, root, R_TASK, 9, JOIST_R_GRANT) == JOIST_OK);
    JT_CHECK(joist_task_set_pager(&k, root, 9, PS, 0) == JOIST_E_RIGHTS); /* no CONTROL */
    JT_CHECK(joist_task_set_pager(&k, root, R_TASK, PS, JOIST_FAULT_NO_THREAD_CAP) == JOIST_OK &&
             k.cs[T].pager_flags == JOIST_FAULT_NO_THREAD_CAP);
    JT_CHECK(joist_task_set_pager(&k, root, R_TASK, JOIST_SLOT_NONE, 0) == JOIST_OK &&
             k.cs[T].pager_slot == 0);
    /* The exception handler's slots are untouched by it. */
    JT_CHECK(k.cs[T].fault_slot == 0 && k.th[w].fault_slot == 0);
    JT_CHECK(clean());
}

JT_TEST(pager_gets_page_faults_and_resumes)
{
    setup_pager(0);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, 0) == JOIST_OK);
    JT_REQUIRE(pager_waits() && handler_waits());
    k.th[w].regs.pc = 0x401100;
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x7000, 6, JOIST_ACCESS_WRITE) == JOIST_BLOCKED);
    JT_CHECK(k.th[w].state == JOIST_T_BLOCKED_REPLY && k.th[w].faulting == 1u &&
             k.th[w].fault_route == JOIST_ROUTE_PAGER);
    const struct joist_msg *m = &k.th[p].msg;
    JT_CHECK(m->r[0] == (JOIST_FAULT_PAGE | JOIST_ACCESS_WRITE << 8) && m->r[1] == 0x7000 &&
             m->r[2] == 6 && m->r[3] == 0x401100 && k.th[p].badge == PAGER_BADGE);
    JT_CHECK(m->ncaps == 1 && m->cap[0] == PCAP); /* flags 0: a thread capability */
    JT_CHECK(k.th[h].state == JOIST_T_BLOCKED_RECV); /* the exception handler saw nothing */
    JT_REQUIRE(verdict(p, JOIST_FAULT_RESUME, 0) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_RUNNING && !k.th[w].faulting && k.th[w].fault_route == 0);
    /* With JOIST_FAULT_NO_THREAD_CAP, as mm sets it: none. */
    JT_REQUIRE(joist_cap_delete(&k, M, PCAP) == JOIST_OK);
    JT_REQUIRE(joist_task_set_pager(&k, root, R_TASK, PS, JOIST_FAULT_NO_THREAD_CAP) == JOIST_OK);
    JT_REQUIRE(pager_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x8000, 4, JOIST_ACCESS_READ) == JOIST_BLOCKED);
    JT_CHECK(k.th[p].msg.ncaps == 0 && k.cs[M].slot[PCAP].obj == 0 && k.th[w].faulting == 2u);
    JT_REQUIRE(verdict(p, JOIST_FAULT_RESUME, 0) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_RUNNING);
    JT_CHECK(clean());
}

JT_TEST(pager_never_sees_other_faults)
{
    setup_pager(JOIST_FAULT_NO_THREAD_CAP);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, 0) == JOIST_OK);
    JT_REQUIRE(pager_waits());
    static const uint64_t kinds[] = {JOIST_FAULT_GP, JOIST_FAULT_UD, JOIST_FAULT_MATH,
                                     JOIST_FAULT_OTHER};
    for (unsigned i = 0; i < 4u; i++) {
        JT_REQUIRE(handler_waits());
        JT_REQUIRE(joist_fault(&k, w, kinds[i], 0, 0, 0) == JOIST_BLOCKED);
        JT_CHECK(k.th[h].msg.r[0] == kinds[i] && k.th[w].fault_route == JOIST_ROUTE_HANDLER);
        JT_CHECK(k.th[p].state == JOIST_T_BLOCKED_RECV);
        JT_REQUIRE(verdict(h, JOIST_FAULT_RESUME, 0) == JOIST_OK);
        JT_REQUIRE(joist_cap_delete(&k, T, HCAP) == JOIST_OK);
    }
    /* With no exception handler a non-page fault ends the task: the pager
     * is never asked. */
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, JOIST_SLOT_NONE, 0) == JOIST_OK);
    JT_CHECK(joist_fault(&k, w, JOIST_FAULT_GP, 0, 0, 0) == JOIST_E_PEER);
    JT_CHECK(!k.cs[T].live && k.th[p].state == JOIST_T_BLOCKED_RECV);
    JT_CHECK(clean());
}

JT_TEST(pager_absent_or_dead_page_faults_go_to_the_handler)
{
    /* No pager: page faults to the exception handler, as before 0027. */
    setup();
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, 0) == JOIST_OK);
    JT_REQUIRE(handler_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x9000, 4, JOIST_ACCESS_READ) == JOIST_BLOCKED);
    JT_CHECK(k.th[h].msg.r[1] == 0x9000 && k.th[w].fault_route == JOIST_ROUTE_HANDLER &&
             (k.th[h].msg.r[0] & JOIST_FAULT_PASSED) == 0);
    JT_REQUIRE(verdict(h, JOIST_FAULT_RESUME, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, T, HCAP) == JOIST_OK);
    /* A pager slot that names nothing usable is no pager. */
    JT_REQUIRE(joist_task_set_pager(&k, root, R_TASK, 9, 0) == JOIST_OK); /* empty */
    JT_REQUIRE(handler_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0xa000, 4, 0) == JOIST_BLOCKED);
    JT_CHECK(k.th[h].msg.r[1] == 0xa000 && k.th[w].fault_route == JOIST_ROUTE_HANDLER);
    JT_REQUIRE(verdict(h, JOIST_FAULT_RESUME, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, T, HCAP) == JOIST_OK);
    /* A dead pager endpoint is no pager either. */
    setup_pager(0);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, 0) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, M, MEP) == JOIST_OK);
    JT_REQUIRE(joist_cap_delete(&k, M, MBADGED) == JOIST_OK); /* the RECV side gone: dead */
    JT_REQUIRE(handler_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0xb000, 4, 0) == JOIST_BLOCKED);
    JT_CHECK(k.th[h].msg.r[1] == 0xb000 && k.th[w].fault_route == JOIST_ROUTE_HANDLER);
    JT_CHECK(clean());
}

JT_TEST(pager_pass_reaches_the_handler_with_the_code)
{
    /* The whole route: mm-like pager (no thread capability), the pass, and
     * the exception handler with a thread capability fixing the thread's
     * registers and resuming it. */
    setup_pager(JOIST_FAULT_NO_THREAD_CAP);
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, FEP, 0) == JOIST_OK);
    JT_REQUIRE(pager_waits() && handler_waits());
    k.th[w].regs.pc = 0x401200;
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0xc000, 7,
                           JOIST_ACCESS_WRITE | JOIST_ACCESS_PRESENT) == JOIST_BLOCKED);
    JT_REQUIRE(k.th[w].fault_route == JOIST_ROUTE_PAGER);
    JT_REQUIRE(verdict(p, JOIST_FAULT_PASS, JOIST_SEGV_ACCERR) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_BLOCKED_REPLY && k.th[w].faulting == 1u &&
             k.th[w].fault_route == JOIST_ROUTE_PASSED);
    const struct joist_msg *m = &k.th[h].msg;
    JT_CHECK(JOIST_FAULT_KIND(m->r[0]) == JOIST_FAULT_PAGE &&
             JOIST_FAULT_ACCESS(m->r[0]) == (JOIST_ACCESS_WRITE | JOIST_ACCESS_PRESENT) &&
             (m->r[0] & JOIST_FAULT_PASSED) != 0 && JOIST_FAULT_CODE(m->r[0]) == JOIST_SEGV_ACCERR);
    JT_CHECK(m->r[1] == 0xc000 && m->r[2] == 7 && m->r[3] == 0x401200);
    JT_CHECK(m->ncaps == 1 && m->cap[0] == HCAP && k.th[h].badge == 0);
    struct joist_regs r;
    JT_REQUIRE(joist_thread_read_regs(&k, T, HCAP, &r) == JOIST_OK);
    r.pc = 0x401300;
    JT_REQUIRE(joist_thread_write_regs(&k, T, HCAP, &r) == JOIST_OK);
    JT_REQUIRE(verdict(h, JOIST_FAULT_RESUME, 0) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_RUNNING && !k.th[w].faulting && k.th[w].fault_route == 0 &&
             k.th[w].regs.pc == 0x401300);
    /* Through the task's exception handler, which asked for no thread
     * capability on page faults: none, and the code still there (only the
     * low 32 bits of what the pager sent). */
    JT_REQUIRE(joist_cap_delete(&k, T, HCAP) == JOIST_OK);
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, JOIST_SLOT_NONE, 0) == JOIST_OK);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, JOIST_FAULT_NO_THREAD_CAP) == JOIST_OK);
    JT_REQUIRE(pager_waits() && handler_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0xd000, 4, JOIST_ACCESS_READ) == JOIST_BLOCKED);
    JT_REQUIRE(verdict(p, JOIST_FAULT_PASS, 0xabc00000000ull | JOIST_SEGV_MAPERR) == JOIST_OK);
    JT_CHECK(k.th[h].msg.ncaps == 0 && k.th[w].faulting == 2u &&
             JOIST_FAULT_CODE(k.th[h].msg.r[0]) == JOIST_SEGV_MAPERR && k.th[h].msg.r[1] == 0xd000);
    JT_REQUIRE(verdict(h, JOIST_FAULT_RESUME, 0) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_RUNNING);
    JT_CHECK(clean());
}

JT_TEST(pager_pass_carries_nothing_of_the_reply)
{
    /* A PASS reply's words and capabilities don't reach the thread, which
     * isn't the one being answered: only the code goes on. */
    setup_pager(JOIST_FAULT_NO_THREAD_CAP);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, 0) == JOIST_OK);
    const joist_slot wrs[] = {11};
    JT_REQUIRE(joist_recv_slots(&k, w, wrs, 1) == JOIST_OK);
    JT_REQUIRE(pager_waits() && handler_waits());
    k.th[w].msg.r[2] = 0x5555;
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0xe000, 4, 0) == JOIST_BLOCKED);
    struct joist_msg m = {0};
    m.r[0] = JOIST_FAULT_PASS;
    m.r[1] = JOIST_BUS_OBJERR;
    m.r[2] = 0x1234;
    m.ncaps = 1;
    m.cap[0] = MBADGED; /* M's: would land in w's slot 11 if delivered */
    m.mask[0] = JOIST_R_SEND;
    JT_REQUIRE(joist_reply(&k, p, &m) == JOIST_OK);
    JT_CHECK(k.cs[T].slot[11].obj == 0 && k.th[w].msg.r[2] == 0x5555);
    JT_CHECK(JOIST_FAULT_CODE(k.th[h].msg.r[0]) == JOIST_BUS_OBJERR && k.th[h].msg.r[2] == 4);
    JT_CHECK(clean());
}

JT_TEST(pager_pass_without_handler_ends_the_task)
{
    setup_pager(JOIST_FAULT_NO_THREAD_CAP);
    JT_REQUIRE(pager_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0xf000, 4, 0) == JOIST_BLOCKED);
    JT_CHECK(verdict(p, JOIST_FAULT_PASS, JOIST_SEGV_MAPERR) == JOIST_OK);
    JT_CHECK(!k.cs[T].live && k.th[w].state == JOIST_T_FREE && k.th[h].state == JOIST_T_FREE);
    JT_CHECK(k.cs[M].live && k.th[p].state == JOIST_T_RUNNING && k.th[p].reply_to == 0);
    JT_CHECK(clean());
}

JT_TEST(pager_pass_to_a_dead_handler_ends_the_task)
{
    setup_pager(JOIST_FAULT_NO_THREAD_CAP);
    JT_REQUIRE(joist_obj_create(&k, T, JOIST_OBJ_ENDPOINT, 7) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, T, 7, 8, JOIST_R_SEND) == JOIST_OK);
    JT_REQUIRE(joist_thread_set_fault(&k, root, R_W, 8, 0) == JOIST_OK);
    JT_REQUIRE(pager_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0xf000, 4, 0) == JOIST_BLOCKED);
    JT_REQUIRE(joist_cap_delete(&k, T, 7) == JOIST_OK); /* the handler's endpoint dies meanwhile */
    JT_CHECK(verdict(p, JOIST_FAULT_PASS, JOIST_SEGV_MAPERR) == JOIST_OK);
    JT_CHECK(!k.cs[T].live && k.th[w].state == JOIST_T_FREE);
    JT_CHECK(clean());
}

JT_TEST(pager_pass_in_the_root_ends_only_the_thread)
{
    /* The root can't go (0013): a passed fault with no exception handler
     * ends the root's thread, as a fault with no handler does. */
    setup_pager(0);
    joist_thread rt;
    JT_REQUIRE(joist_thread_create(&k, root, &rt) == JOIST_OK);
    JT_REQUIRE(joist__cap_grant(&k, M, MBADGED, root, 20, JOIST_R_SEND) == JOIST_OK);
    k.cs[root].pager_slot = 21; /* the root has no task capability to itself: set directly */
    JT_REQUIRE(pager_waits());
    JT_REQUIRE(joist_fault(&k, rt, JOIST_FAULT_PAGE, 0x1000, 4, 0) == JOIST_BLOCKED);
    JT_CHECK(verdict(p, JOIST_FAULT_PASS, JOIST_SEGV_MAPERR) == JOIST_OK);
    JT_CHECK(k.cs[root].live && k.th[rt].state == JOIST_T_FREE);
    JT_CHECK(clean());
}

JT_TEST(pager_pass_at_most_once)
{
    /* A PASS from the exception handler terminates the thread, whether the
     * fault came to it directly or passed. */
    setup_pager(JOIST_FAULT_NO_THREAD_CAP);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, JOIST_FAULT_NO_THREAD_CAP) == JOIST_OK);
    JT_REQUIRE(pager_waits() && handler_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x2000, 4, 0) == JOIST_BLOCKED);
    JT_REQUIRE(verdict(p, JOIST_FAULT_PASS, JOIST_SEGV_MAPERR) == JOIST_OK);
    JT_REQUIRE(pager_waits()); /* the pager waits again: it must not get this fault back */
    JT_CHECK(verdict(h, JOIST_FAULT_PASS, JOIST_SEGV_MAPERR) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_FREE && k.th[p].state == JOIST_T_BLOCKED_RECV);
    JT_CHECK(k.cs[T].live); /* the thread, not the task: a verdict, not a missing handler */
    /* Directly: a GP to the handler, which answers PASS. */
    joist_thread w2;
    JT_REQUIRE(joist_thread_spawn(&k, root, R_TASK, 12, &w2) == JOIST_OK);
    JT_REQUIRE(handler_waits());
    JT_REQUIRE(joist_fault(&k, w2, JOIST_FAULT_GP, 0, 0, 0) == JOIST_BLOCKED);
    JT_REQUIRE(joist_cap_delete(&k, T, HCAP) == JOIST_OK);
    JT_CHECK(verdict(h, JOIST_FAULT_PASS, JOIST_SEGV_MAPERR) == JOIST_OK);
    JT_CHECK(k.th[w2].state == JOIST_T_FREE && k.th[p].state == JOIST_T_BLOCKED_RECV);
    JT_CHECK(clean());
}

JT_TEST(pager_other_verdicts_as_before)
{
    /* Anything but RESUME and PASS from the pager ends the thread, as any
     * handler's verdict did before 0027. */
    setup_pager(JOIST_FAULT_NO_THREAD_CAP);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, 0) == JOIST_OK);
    JT_REQUIRE(pager_waits() && handler_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x3000, 4, 0) == JOIST_BLOCKED);
    JT_CHECK(verdict(p, 1, 0) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_FREE && k.th[h].state == JOIST_T_BLOCKED_RECV);
    JT_CHECK(clean());
}

JT_TEST(pager_pass_queues_when_the_handler_is_busy)
{
    /* The exception handler isn't receiving: the passed fault waits on its
     * endpoint, and arrives, still passed, with its thread capability. */
    setup_pager(JOIST_FAULT_NO_THREAD_CAP);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, 0) == JOIST_OK);
    JT_REQUIRE(pager_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x4000, 4, JOIST_ACCESS_READ) == JOIST_BLOCKED);
    JT_REQUIRE(verdict(p, JOIST_FAULT_PASS, JOIST_SEGV_MAPERR) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_BLOCKED_SEND && k.th[w].fault_route == JOIST_ROUTE_PASSED);
    JT_REQUIRE(joist_recv(&k, h, FEP, JOIST_FOREVER) == JOIST_OK);
    JT_CHECK((k.th[h].msg.r[0] & JOIST_FAULT_PASSED) != 0 && k.th[h].msg.r[1] == 0x4000 &&
             k.th[h].msg.cap[0] == HCAP);
    JT_REQUIRE(verdict(h, JOIST_FAULT_RESUME, 0) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_RUNNING);
    /* A passed fault left unanswered (the handler gone) leaves the thread
     * suspended, as any fault call does. */
    JT_REQUIRE(joist_cap_delete(&k, T, HCAP) == JOIST_OK);
    JT_REQUIRE(pager_waits() && handler_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x4000, 4, 0) == JOIST_BLOCKED);
    JT_REQUIRE(verdict(p, JOIST_FAULT_PASS, JOIST_SEGV_MAPERR) == JOIST_OK);
    JT_REQUIRE(joist_thread_kill(&k, root, R_H) == JOIST_OK);
    JT_CHECK(k.th[w].state == JOIST_T_SUSPENDED && !k.th[w].faulting && k.th[w].fault_route == 0);
    JT_CHECK(clean());
}

JT_TEST(pager_pass_through_a_saved_reply)
{
    /* A pager that saves the fault's reply (0001's reply_save, as 0024's
     * helpers will) passes it on later in the same way. */
    setup_pager(JOIST_FAULT_NO_THREAD_CAP);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, 0) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, M, JOIST_OBJ_REPLY, 8) == JOIST_OK);
    JT_REQUIRE(pager_waits() && handler_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x5000, 4, 0) == JOIST_BLOCKED);
    JT_REQUIRE(joist_reply_save(&k, p, 8) == JOIST_OK);
    JT_REQUIRE(pager_waits()); /* serving others meanwhile */
    struct joist_msg m = {0};
    m.r[0] = JOIST_FAULT_PASS;
    m.r[1] = JOIST_BUS_ADRERR;
    JT_REQUIRE(joist_reply_saved(&k, p, 8, &m) == JOIST_E_STATE); /* p is receiving */
    joist_thread p2;
    JT_REQUIRE(joist_thread_spawn(&k, root, R_M, 13, &p2) == JOIST_OK);
    JT_REQUIRE(joist_reply_saved(&k, p2, 8, &m) == JOIST_OK);
    JT_CHECK(k.th[w].fault_route == JOIST_ROUTE_PASSED &&
             JOIST_FAULT_CODE(k.th[h].msg.r[0]) == JOIST_BUS_ADRERR && k.th[h].msg.r[1] == 0x5000);
    JT_CHECK(clean());
}

JT_TEST(pager_pass_charged_to_the_faulting_thread)
{
    /* Both calls run on the faulting thread's context (0009, 0027). */
    setup_pager(JOIST_FAULT_NO_THREAD_CAP);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, 0) == JOIST_OK);
    JT_REQUIRE(joist_sc_create(&k, root, R_SC, 9, 2, 4) == JOIST_OK);
    JT_REQUIRE(joist_sc_bind(&k, root, R_SC, R_W) == JOIST_OK);
    uint32_t sc = k.cs[root].slot[R_SC].obj;
    JT_REQUIRE(pager_waits() && handler_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x6000, 4, 0) == JOIST_BLOCKED);
    JT_CHECK(k.th[p].run_sc == sc && k.th[w].run_sc == 0);
    JT_REQUIRE(verdict(p, JOIST_FAULT_PASS, JOIST_SEGV_MAPERR) == JOIST_OK);
    JT_CHECK(k.th[h].run_sc == sc && k.th[p].run_sc == 0 && k.th[w].run_sc == 0);
    JT_REQUIRE(verdict(h, JOIST_FAULT_RESUME, 0) == JOIST_OK);
    JT_CHECK(k.th[w].run_sc == sc && k.th[h].run_sc == 0);
    JT_CHECK(clean());
}

JT_TEST(pager_in_the_task_itself_passing_with_no_handler)
{
    /* A pager thread of the faulting task's own, with no exception handler:
     * the pass ends the task, the pager with it, in the middle of its own
     * reply. Nothing is left half done. */
    setup();
    JT_REQUIRE(joist_task_set_pager(&k, root, R_TASK, FEP, JOIST_FAULT_NO_THREAD_CAP) == JOIST_OK);
    JT_REQUIRE(handler_waits()); /* h is the pager here */
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x7000, 4, 0) == JOIST_BLOCKED);
    JT_REQUIRE(k.th[w].fault_route == JOIST_ROUTE_PAGER);
    JT_CHECK(joist_reply_recv(&k, h, &(struct joist_msg){.r = {JOIST_FAULT_PASS, 1}}, FEP,
                              JOIST_FOREVER) == JOIST_E_STATE); /* h is gone by its receive */
    JT_CHECK(!k.cs[T].live && k.th[w].state == JOIST_T_FREE && k.th[h].state == JOIST_T_FREE);
    JT_CHECK(clean());
}

JT_TEST(pager_and_handler_on_one_endpoint)
{
    /* One endpoint in both roles (allowed: the kernel doesn't look). The
     * pass still comes back marked passed, and a second PASS ends it. */
    setup();
    JT_REQUIRE(joist_task_set_pager(&k, root, R_TASK, FEP, JOIST_FAULT_NO_THREAD_CAP) == JOIST_OK);
    JT_REQUIRE(joist_task_set_fault(&k, root, R_TASK, FEP, JOIST_FAULT_NO_THREAD_CAP) == JOIST_OK);
    JT_REQUIRE(handler_waits());
    JT_REQUIRE(joist_fault(&k, w, JOIST_FAULT_PAGE, 0x8000, 4, 0) == JOIST_BLOCKED);
    JT_REQUIRE((k.th[h].msg.r[0] & JOIST_FAULT_PASSED) == 0);
    JT_REQUIRE(joist_reply_recv(&k, h, &(struct joist_msg){.r = {JOIST_FAULT_PASS, 1}}, FEP,
                                JOIST_FOREVER) == JOIST_OK);
    JT_CHECK((k.th[h].msg.r[0] & JOIST_FAULT_PASSED) != 0 &&
             k.th[w].fault_route == JOIST_ROUTE_PASSED);
    JT_CHECK(verdict(h, JOIST_FAULT_PASS, 1) == JOIST_OK && k.th[w].state == JOIST_T_FREE);
    JT_CHECK(clean());
}
