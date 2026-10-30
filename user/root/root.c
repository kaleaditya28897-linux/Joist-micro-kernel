/* SPDX-License-Identifier: GPL-3.0-only */
/* root.c - the root task (decision 0013), as far as Phase 3 step 4 needs
 * it: it is handed all the memory and the boot modules as frame
 * capabilities, and its job is to partition and delegate. It loads each
 * module as a task of its own (lib/loader.h: the kernel's ELF validator
 * built for user mode, the task's memory a page at a time from the pool),
 * hands it its capabilities, and starts its thread.
 *
 * Here: module 0 is pong, which gets the endpoint to receive on; module 1 is
 * ping, which gets a badged send capability to it and one to the endpoint
 * the root waits on for ping's report.
 *
 * Pong is a passive server: it runs only on the time each call lends it
 * (§5.3), at its caller's priority and on its caller's budget. But it needs
 * time of its own once, to reach its first Recv. So the root gives it a
 * context for its start, calls it once to know that it is in its loop, and
 * takes the context back. (Until step 9 the context's priority, above the
 * root's, did the sequencing; with more than one CPU the root would go on
 * while pong started.) */
#include "../lib/loader.h"

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free);

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 2) {
        die("needs two modules: pong and ping");
    }
    uint64_t pong_task = slot_new();
    uint64_t ping_task = slot_new();
    uint64_t pong_entry = ld_load(0, pong_task);
    uint64_t ping_entry = ld_load(1, ping_task);

    /* The endpoint pong serves, and the one ping reports on. */
    uint64_t ep = slot_new();
    uint64_t done = slot_new();
    uint64_t tmp = slot_new();
    must(j_obj_create(J_ENDPOINT, ep), "obj_create");
    must(j_obj_create(J_ENDPOINT, done), "obj_create");
    must(j_cap_give(pong_task, ep, 0, J_R_RECV), "cap_give (pong's endpoint)");
    must(j_cap_mint(ep, tmp, J_R_SEND | J_R_GRANT, 7), "cap_mint");
    must(j_cap_give(ping_task, tmp, 0, J_R_SEND), "cap_give (ping's endpoint)");
    must(j_cap_delete(tmp), "cap_delete");
    must(j_cap_mint(done, tmp, J_R_SEND | J_R_GRANT, 1), "cap_mint");
    must(j_cap_give(ping_task, tmp, 1, J_R_SEND), "cap_give (ping's report)");
    must(j_cap_delete(tmp), "cap_delete");

    uint64_t pong_thread = slot_new();
    uint64_t ping_thread = slot_new();
    uint64_t init_sc = slot_new();
    uint64_t ping_sc = slot_new();
    must(j_thread_spawn(pong_task, pong_thread, pong_entry, CHILD_LO), "thread_spawn (pong)");
    must(j_sc_create(init_sc, 200, 10 * J_MS, 10 * J_MS), "sc_create (init)");
    must(j_sc_bind(init_sc, pong_thread), "sc_bind (pong)");
    /* Wait until pong is in its loop: one call of the root's own. Its
     * answer comes from a ReplyRecv, which replies and waits in one step,
     * so when the answer is here pong is waiting, on any number of CPUs. */
    struct j_msg hello = {.r = {41}};
    must(j_cap_mint(ep, tmp, J_R_SEND, 7), "cap_mint");
    must(j_call(tmp, &hello, J_FOREVER), "call (pong, are you there)");
    must(j_cap_delete(tmp), "cap_delete");
    if (hello.r[0] != 42) {
        die("pong's first answer is wrong");
    }
    must(j_sc_unbind(init_sc), "sc_unbind (pong)"); /* passive from here */
    must(j_thread_spawn(ping_task, ping_thread, ping_entry, CHILD_LO), "thread_spawn (ping)");
    must(j_sc_create(ping_sc, 100, 10 * J_MS, 10 * J_MS), "sc_create (ping)");
    must(j_sc_bind(ping_sc, ping_thread), "sc_bind (ping)");
    j_puts("root: pong and ping started\n");

    struct j_msg m = {0};
    must(j_recv(done, &m, J_FOREVER), "recv (report)");
    j_puts("root: ping reported ");
    j_putu(m.r[0]);
    j_puts(" round trips\n");
    m.caps = 0;
    (void)j_reply(&m);
    j_exit(0);
}
