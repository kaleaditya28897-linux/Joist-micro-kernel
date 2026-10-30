/* SPDX-License-Identifier: GPL-3.0-only */
/* scaleroot.c - a root task that measures how IPC throughput scales with
 * CPUs under the one kernel lock (decision 0016; the lock study of
 * 2026-10-15). Module 0 is ipcbench, which is both ends.
 *
 * K independent client/server pairs, each pair two tasks of its own, K = 1
 * to the number of CPUs. Every client calls its own server as fast as it
 * can, all from the same moment for a fixed time (by the TSC, which every
 * CPU shares), and reports how many round trips it made; this prints them,
 * per pair and in total. Two modes:
 * - the server passive: a pair runs on its client's context, on one CPU,
 *   through the direct switch (the fast path);
 * - the server with a context of its own, above its client's: every call
 *   and reply is a change of context for the scheduler.
 *
 * Lines (one per pair, then the total):
 *   scale: MODE k=K pair=I n=N late=L ruler=R
 *   scale: MODE k=K total=N per_s=... window=W
 * where N is round trips in the window of W TSC counts, and R is the
 * ruler's mean counts for 12 000 CPU cycles (ipcbench), so a round trip
 * takes W * 12000 / (R * N) cycles of that CPU's clock.
 *
 * On a kernel built with RUN_LOCK_STATS the kernel lock is also counted
 * over each window and printed after it (`lockstats:` lines). */
#include "../lib/loader.h"
#include "../lib/work.h"

#ifndef SCALE_WINDOW_MS
#define SCALE_WINDOW_MS 2000u
#endif
#ifndef SCALE_REPS
#define SCALE_REPS 1u
#endif
#define SCALE_LEAD_MS 300u /* from the first pair's start to the window */
#define MAX_PAIRS 8u

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus);

static uint64_t done;
static uint64_t per_ms; /* TSC counts in a millisecond */

static void put(const char *what, uint64_t v)
{
    j_puts(what);
    j_putu(v);
}

static void sleep_ms(uint64_t ms)
{
    struct j_msg m = {0};
    if (j_recv(done, &m, ms * J_MS) != J_E_TIMEOUT) {
        die("a sleep didn't time out");
    }
}

static void thread_start(uint64_t task, uint64_t thread, uint64_t entry, uint64_t role,
                         uint64_t name, uint64_t from, uint64_t to)
{
    struct j_thread_regs regs;
    must(j_thread_spawn(task, thread, entry, CHILD_LO), "thread_spawn");
    must(j_thread_read_regs(thread, &regs), "thread_read_regs");
    regs.r[J_RDI] = role;
    regs.r[J_RSI] = name;
    regs.r[J_RDX] = from;
    regs.r[J_RCX] = to;
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

struct pair {
    uint64_t stask, ctask, ep, sthread, cthread, ssc, csc;
};

static void pair_start(struct pair *p, uint64_t i, int passive, uint64_t from, uint64_t to)
{
    uint64_t tmp = slot_new();
    p->stask = slot_new();
    p->ctask = slot_new();
    p->ep = slot_new();
    p->sthread = slot_new();
    p->cthread = slot_new();
    p->ssc = slot_new();
    p->csc = slot_new();
    uint64_t entry = ld_load(0, p->stask);
    (void)ld_load(0, p->ctask);
    must(j_obj_create(J_ENDPOINT, p->ep), "obj_create");
    must(j_cap_give(p->stask, p->ep, 0, J_R_RECV), "cap_give (server)");
    must(j_cap_give(p->ctask, p->ep, 1, J_R_SEND), "cap_give (client)");
    must(j_cap_mint(done, tmp, J_R_SEND | J_R_GRANT, i + 1u), "cap_mint");
    must(j_cap_give(p->ctask, tmp, 2, J_R_SEND), "cap_give (done)");
    must(j_cap_delete(tmp), "cap_delete");
    thread_start(p->stask, p->sthread, entry, 0, 0, 0, 0);
    thread_start(p->ctask, p->cthread, entry, 3, i, from, to);
    /* The server reaches its Recv on a context of its own; a passive one
     * then gives it up, after answering one call (docs/abi.md: the
     * start-up handshake, which holds on any number of CPUs). */
    must(j_sc_create(p->ssc, 200, 10 * J_MS, 10 * J_MS), "sc_create (server)");
    must(j_sc_bind(p->ssc, p->sthread), "sc_bind (server)");
    struct j_msg m = {0};
    must(j_call(p->ep, &m, J_FOREVER), "call (server start)");
    if (passive) {
        must(j_sc_unbind(p->ssc), "sc_unbind (server)");
    }
    must(j_sc_create(p->csc, 100, 10 * J_MS, 10 * J_MS), "sc_create (client)");
    must(j_sc_bind(p->csc, p->cthread), "sc_bind (client)");
}

static void pair_end(struct pair *p)
{
    finish(p->stask);
    finish(p->ctask);
    must(j_cap_delete(p->ep), "cap_delete");
    must(j_cap_delete(p->ssc), "cap_delete");
    must(j_cap_delete(p->csc), "cap_delete");
    must(j_cap_delete(p->sthread), "cap_delete");
    must(j_cap_delete(p->cthread), "cap_delete");
}

static void measure(uint64_t k, int passive)
{
    const char *mode = passive ? "passive" : "own-context";
    struct pair p[MAX_PAIRS];
    uint64_t n[MAX_PAIRS] = {0};
    uint64_t window = SCALE_WINDOW_MS * per_ms;
    uint64_t from = work_tsc() + SCALE_LEAD_MS * per_ms;
    uint64_t to = from + window;
    (void)j_sys4(99, 0, from, to, 0); /* the kernel lock's counts (RUN_LOCK_STATS kernels only) */
    for (uint64_t i = 0; i < k; i++) {
        pair_start(&p[i], i, passive, from, to);
    }
    uint64_t total = 0;
    for (uint64_t got = 0; got < k; got++) {
        struct j_msg m = {0};
        must(j_recv(done, &m, J_FOREVER), "recv (done)");
        uint64_t i = m.badge - 1u;
        if (i >= k || n[i] != 0) {
            die("a report from nowhere");
        }
        n[i] = m.r[0];
        total += m.r[0];
        j_puts("scale: ");
        j_puts(mode);
        put(" k=", k);
        put(" pair=", i);
        put(" n=", m.r[0]);
        put(" late=", m.r[3]);
        put(" ruler=", m.r[2] != 0 ? m.r[1] / m.r[2] : 0);
        j_puts("\n");
        m.caps = 0;
        (void)j_reply(&m);
    }
    (void)j_sys4(99, 1, 0, 0, 0);
    j_puts("scale: ");
    j_puts(mode);
    put(" k=", k);
    put(" total=", total);
    put(" per_s=", total * 1000u / SCALE_WINDOW_MS);
    put(" window=", window);
    j_puts("\n");
    for (uint64_t i = 0; i < k; i++) {
        pair_end(&p[i]);
    }
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
    /* Settle (as benchroot), then the TSC's rate against the kernel's
     * clock: a sleep of 500 ms. */
    sleep_ms(1000);
    uint64_t t0 = work_tsc();
    sleep_ms(500);
    per_ms = (work_tsc() - t0) / 500u;
    put("scale: cpus=", ncpus);
    put(" tsc_per_ms=", per_ms);
    put(" window_ms=", SCALE_WINDOW_MS);
    j_puts("\n");
    uint64_t kmax = ncpus < MAX_PAIRS ? ncpus : MAX_PAIRS;
    for (uint64_t rep = 0; rep < SCALE_REPS; rep++) {
        for (int passive = 1; passive >= 0; passive--) {
            for (uint64_t k = 1; k <= kmax; k++) {
                measure(k, passive);
            }
        }
    }
    j_puts("scale: done\n");
    j_exit(0);
}
