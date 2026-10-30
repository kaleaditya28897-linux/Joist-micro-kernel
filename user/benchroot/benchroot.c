/* SPDX-License-Identifier: GPL-3.0-only */
/* benchroot.c - a root task that runs the IPC benchmark (Phase 3 step 8;
 * docs/phase3-plan.md, *Testing*: round-trip Call/ReplyRecv latency in TSC
 * cycles, in one address space and across two). Module 0 is ipcbench, which
 * is both ends; this sets each measurement up, waits for the client to say
 * it has printed its figures, and clears it away for the next.
 *
 * Three measurements:
 * - two tasks, the server passive: it runs on the client's scheduling
 *   context for the length of each call (the ordinary case, §5.3);
 * - the same with both threads in one task: no address-space switch;
 * - two tasks, the server with a scheduling context of its own, above the
 *   client's: every call and reply is a change of context for the
 *   scheduler to make;
 * - two tasks, the server passive, with the whole 4 KiB IPC buffer
 *   carried both ways;
 * - on more than one CPU, the first again while every other CPU makes
 *   system calls as fast as it can: the kernel lock, contended. */
#include "../lib/loader.h"

#ifndef BENCH_SETTLE_MS
#define BENCH_SETTLE_MS 1000u
#endif
#define STACK2_TOP 0x500000ull /* the second thread's stack, in one task */

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus);

static uint64_t done;

static void thread_start(uint64_t task, uint64_t thread, uint64_t entry, uint64_t sp,
                         uint64_t role, uint64_t name)
{
    struct j_thread_regs regs;
    must(j_thread_spawn(task, thread, entry, sp), "thread_spawn");
    must(j_thread_read_regs(thread, &regs), "thread_read_regs");
    regs.r[J_RDI] = role;
    regs.r[J_RSI] = name;
    must(j_thread_write_regs(thread, &regs), "thread_write_regs");
}

static void finish(uint64_t task)
{
    int more = 1;
    must(j_task_destroy(task), "task_destroy");
    must(j_cap_delete(task), "cap_delete");
    while (more) {
        must(j_continue(0, &more), "continue");
    }
}

static void measure(uint64_t name, int one_task, int passive)
{
    uint64_t stask = slot_new();
    uint64_t ctask = one_task ? stask : slot_new();
    uint64_t ep = slot_new();
    uint64_t tmp = slot_new();
    uint64_t sthread = slot_new();
    uint64_t cthread = slot_new();
    uint64_t ssc = slot_new();
    uint64_t csc = slot_new();
    uint64_t entry = ld_load(0, stask);
    if (one_task) {
        ld_stack(stask, STACK2_TOP);
    } else {
        (void)ld_load(0, ctask);
    }
    must(j_obj_create(J_ENDPOINT, ep), "obj_create");
    must(j_cap_give(stask, ep, 0, J_R_RECV), "cap_give (server)");
    must(j_cap_give(ctask, ep, 1, J_R_SEND), "cap_give (client)");
    must(j_cap_mint(done, tmp, J_R_SEND | J_R_GRANT, name + 1u), "cap_mint");
    must(j_cap_give(ctask, tmp, 2, J_R_SEND), "cap_give (done)");
    must(j_cap_delete(tmp), "cap_delete");

    thread_start(stask, sthread, entry, CHILD_LO, 0, name);
    thread_start(ctask, cthread, entry, one_task ? STACK2_TOP : CHILD_LO, 1, name);
    /* The server reaches its Recv on a context above the root's; a passive
     * one then gives it up. */
    must(j_sc_create(ssc, 200, 10 * J_MS, 10 * J_MS), "sc_create (server)");
    must(j_sc_bind(ssc, sthread), "sc_bind (server)");
    if (passive) {
        /* The start-up handshake (docs/abi.md): the server answers one
         * call, so it is waiting in its Recv before its context goes. A
         * higher priority alone sequences this only on one CPU; on 4 the
         * server sometimes never got there (lock study, 2026-10-15). */
        struct j_msg h = {0};
        must(j_call(ep, &h, J_FOREVER), "call (server start)");
        must(j_sc_unbind(ssc), "sc_unbind (server)");
    }
    must(j_sc_create(csc, 100, 10 * J_MS, 10 * J_MS), "sc_create (client)");
    must(j_sc_bind(csc, cthread), "sc_bind (client)");

    struct j_msg m = {0};
    must(j_recv(done, &m, J_FOREVER), "recv (done)");
    if (m.badge != name + 1u) {
        die("a report from the wrong measurement");
    }
    m.caps = 0;
    (void)j_reply(&m);
    finish(stask);
    if (!one_task) {
        finish(ctask);
    }
    must(j_cap_delete(ep), "cap_delete");
    must(j_cap_delete(ssc), "cap_delete");
    must(j_cap_delete(csc), "cap_delete");
    must(j_cap_delete(sthread), "cap_delete");
    must(j_cap_delete(cthread), "cap_delete");
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 1) {
        die("needs one module: ipcbench");
    }
    done = slot_new();
    must(j_obj_create(J_ENDPOINT, done), "obj_create");
    /* Let the machine settle: for the first few tenths of a second after a
     * virtual machine starts, its host is still busy with starting it, and
     * the first measurement came out half as slow again in two runs of
     * five. (A Recv nobody answers, with a timeout: a sleep.) */
    struct j_msg m = {0};
    if (j_recv(done, &m, BENCH_SETTLE_MS * J_MS) != J_E_TIMEOUT) {
        die("the settling sleep didn't time out");
    }
    measure(0, 0, 1);
    measure(1, 1, 1);
    measure(2, 0, 0);
    measure(4, 0, 1); /* the IPC buffer, full, both ways (Phase 3's exit) */
    if (ncpus > 1) {
        /* The kernel lock under contention (decision 0016 asks for its cost
         * on 2 and 4 CPUs): the first measurement again while a thread on
         * every other CPU does nothing but enter the kernel. */
        for (uint64_t i = 1; i < ncpus; i++) {
            uint64_t task = slot_new();
            uint64_t thread = slot_new();
            uint64_t sc = slot_new();
            uint64_t entry = ld_load(0, task);
            thread_start(task, thread, entry, CHILD_LO, 2, 0);
            must(j_sc_create(sc, 100, 10 * J_MS, 10 * J_MS), "sc_create (busy)");
            must(j_sc_bind(sc, thread), "sc_bind (busy)");
        }
        measure(3, 0, 1);
    }
    j_puts("ipcbench: done\n");
    j_exit(0);
}
