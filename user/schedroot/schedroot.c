/* SPDX-License-Identifier: GPL-3.0-only */
/* schedroot.c - a root task that tests the scheduler from user mode (Phase
 * 3 step 5; docs/abi.md, *Scheduling*). Module 0 is worker, loaded here as
 * several tasks. Each shares a page with the root (lib/work.h): the root
 * writes its role there, and reads its count, which only moves while the
 * worker runs. So who ran, and for what share of the time, can be read off
 * without a clock.
 *
 * The root has priority 128 (the kernel gives it that). Workers below it
 * run only while it sleeps (a Recv with a timeout on an endpoint nobody
 * sends to), and waking, it preempts them: they never call the kernel.
 *
 * Shares are checked against wide bounds: under an emulator on a busy host
 * time is uneven, and what is being told apart is a fifth from all or
 * nothing. */
#define TEST_NAME "sched"
#include "../lib/testroot.h"

#define SLEEP_NS     (300 * J_MS)
#define BUSY_COUNTS  30000000u
#ifndef SERVE_COUNTS
#define SERVE_COUNTS 200000u /* a request's work: enough that the call itself is small beside it */
#endif

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus);

/* Be busy without calling the kernel, for longer than the root's budget. */
static void busy(void)
{
    static struct work mine;
    work_spin(&mine, BUSY_COUNTS);
}

static uint64_t percent(uint64_t a, uint64_t b)
{
    return a + b == 0 ? 0 : a * 100u / (a + b);
}

/* A result, with the two counts behind it (the raw data, for the record). */
static void report(const char *what, uint64_t pct, uint64_t a, uint64_t b)
{
    j_puts("sched: ");
    j_puts(what);
    j_puts(" ");
    j_putu(pct);
    j_puts("% (counts ");
    j_putu(a);
    j_puts(", ");
    j_putu(b);
    j_puts(")\n");
}

/* 1. Fixed priorities, and preemption on waking. */
static void test_priority(void)
{
    struct worker low = worker_new(WORK_SPIN, 0);
    worker_run(&low, 50, 10 * J_MS, 10 * J_MS);
    busy();
    check(low.w->count == 0, "a lower priority ran while the root could");
    sleep_ns(SLEEP_NS); /* now it runs; the timeout takes the CPU back from it */
    uint64_t c = low.w->count;
    check(c != 0, "a lower priority didn't run while the root slept");
    busy();
    check(low.w->count == c, "a lower priority ran after the root woke");

    /* The other way: a higher priority runs the moment it can, and the
     * root gets the CPU back only when its budget is spent. */
    struct worker high = worker_new(WORK_SPIN, 0);
    worker_run(&high, 200, 2 * J_MS, 10 * J_MS);
    check(high.w->count != 0, "a higher priority didn't run at once");
    worker_stop(&high);
    worker_stop(&low);
    c = low.w->count;
    uint64_t h = high.w->count;
    sleep_ns(SLEEP_NS / 10);
    check(low.w->count == c && high.w->count == h, "a thread ran without a context");
    j_puts("sched: priorities ok\n");
}

/* 2. A budget is enforced: 2 ms in every 10, over a lower priority that
 * takes the rest. Without enforcement the higher one would take it all. */
static void test_budget(void)
{
    struct worker a = worker_new(WORK_SPIN, 0);
    struct worker b = worker_new(WORK_SPIN, 0);
    worker_run(&b, 50, 10 * J_MS, 10 * J_MS);
    worker_run(&a, 60, 2 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS);
    uint64_t pct = percent(a.w->count, b.w->count);
    worker_stop(&a);
    worker_stop(&b);
    check(b.w->count != 0, "a spent budget kept the CPU from a lower priority");
    check(pct >= 8 && pct <= 40, "2 ms in 10 wasn't about a fifth of the time");
    report("budget ok: 2 ms in every 10 got", pct, a.w->count, b.w->count);
}

/* 3. Time in the kernel is charged too: a thread that does nothing but
 * make system calls, with 2 ms in every 10, leaves a lower priority about
 * four fifths of what it gets alone. */
static void test_kernel_time(void)
{
    struct worker b = worker_new(WORK_SPIN, 0);
    struct worker a = worker_new(WORK_CALLS, 0);
    worker_run(&b, 50, 10 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS);
    uint64_t alone = b.w->count;
    worker_run(&a, 60, 2 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS);
    uint64_t beside = b.w->count - alone;
    worker_stop(&a);
    worker_stop(&b);
    check(alone != 0 && a.w->count != 0, "a worker didn't run");
    uint64_t pct = beside * 100u / alone;
    check(pct >= 60 && pct <= 92, "a budget spent in system calls wasn't about a fifth of the time");
    report("kernel time ok: beside 2 ms in every 10 of system calls, a spinner kept", pct, beside, alone);
}

/* 4. Equals take turns. */
static void test_equals(void)
{
    struct worker a = worker_new(WORK_SPIN, 0);
    struct worker b = worker_new(WORK_SPIN, 0);
    worker_run(&a, 50, 10 * J_MS, 10 * J_MS);
    worker_run(&b, 50, 10 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS);
    uint64_t pct = percent(a.w->count, b.w->count);
    worker_stop(&a);
    worker_stop(&b);
    check(pct >= 30 && pct <= 70, "two equals didn't share the time about evenly");
    report("equals ok: one of two got", pct, a.w->count, b.w->count);
}

/* 5. Donation: a passive server works on its client's time. The client
 * has 2 ms in every 10 at priority 60; the server, with no context, does
 * all the work, above a spinner at 50 and out of the client's budget: the
 * spinner keeps about four fifths of what it gets alone. */
static void test_donation(void)
{
    struct worker server = worker_new(WORK_SERVE, SERVE_COUNTS);
    struct worker client = worker_new(WORK_CLIENT, 0);
    struct worker other = worker_new(WORK_SPIN, 0);
    uint64_t ep = slot_new();
    must(j_obj_create(J_ENDPOINT, ep), "obj_create");
    must(j_cap_give(server.task, ep, 0, J_R_RECV), "cap_give (server)");
    must(j_cap_give(client.task, ep, 0, J_R_SEND), "cap_give (client)");
    /* The server's start: a context above the root's until its first Recv. */
    worker_run(&server, 200, 10 * J_MS, 10 * J_MS);
    worker_stop(&server);
    check(server.w->count == 0, "the server worked before anyone called");
    worker_run(&other, 50, 10 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS);
    check(server.w->count == 0, "a passive server ran with no caller");
    uint64_t alone = other.w->count;
    check(alone != 0, "the spinner didn't run");
    worker_run(&client, 60, 2 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS);
    uint64_t s = server.w->count;
    uint64_t calls = client.w->count;
    uint64_t beside = other.w->count - alone;
    check(s != 0 && calls != 0, "the server never ran on its client's time");
    uint64_t kept = beside * 100u / alone;
    uint64_t pct = percent(s, beside);
    check(kept >= 60 && kept <= 92, "a server's work wasn't limited by its client's budget");
    check(pct >= 8 && pct <= 40, "work on donated time wasn't about the client's fifth");
    j_puts("sched: ");
    j_putu(calls);
    j_puts(" calls served;\n");
    report("donation ok: a passive server on its client's 2 ms in every 10 got", pct, s, beside);
    report("donation ok: and the spinner beside them kept", kept, beside, alone);
}

/* 6. The clock (Phase 5 step 7: what glibc's clock_gettime stands on):
 * it says how many CPUs there are, it never goes back, and a sleep of 5 ms
 * reads as at least 5 ms (and not wildly more). */
static void test_clock(uint64_t ncpus)
{
    uint64_t t0, cpus;
    check(j_clock(&t0, &cpus) == J_OK, "clock refused");
    check(cpus == ncpus, "clock's CPU count isn't the kernel's");
    uint64_t last = t0;
    for (unsigned i = 0; i < 1000u; i++) {
        uint64_t now, c;
        j_clock(&now, &c);
        check(now >= last, "the clock went back");
        last = now;
    }
    sleep_ns(5 * J_MS);
    uint64_t t1;
    j_clock(&t1, &cpus);
    check(t1 - t0 >= 5 * J_MS, "a 5 ms sleep read as less");
    check(t1 - t0 < 1000 * J_MS, "a 5 ms sleep read as over a second");
    j_puts("sched: clock ok: a 5 ms sleep read as ");
    j_putu((t1 - t0) / 1000u);
    j_puts(" us, CPUs ");
    j_putu(cpus);
    j_puts("\n");
}

/* 7. A thread sets its own TLS base (0010; glibc's TLS_INIT_TP on Joist):
 * it reads its own register record, points the TLS base at a block of its
 * memory and writes the record back, which resumes it after the read with
 * the new base: %fs:0 then reads the block's first word. */
static volatile uint64_t tls_block[2] = {0x6a6f697374u, 0};
static volatile uint64_t tls_seen;
static uint64_t tls_slot;
static uint8_t tls_stack[4096] __attribute__((aligned(16)));

static void tls_thread(void)
{
    struct j_thread_regs r;
    /* The record holds the pc the read returns to: written back, the
     * thread resumes there, every register as then. A flag in memory
     * (which the write doesn't restore) tells the second arrival from the
     * first. */
    volatile int written = 0;
    uint64_t v = 0, e1 = j_thread_read_regs(tls_slot, &r), e2 = 0;
    if (e1 == J_OK && !written) {
        written = 1;
        r.tls = (uint64_t)(uintptr_t)tls_block;
        e2 = j_thread_write_regs(tls_slot, &r); /* goes on after the read */
    }
    if (e1 == J_OK && e2 == J_OK) {
        __asm__ volatile("mov %%fs:0, %0" : "=r"(v));
    }
    tls_seen = v != 0 ? v : 1000u + e1 * 100u + e2;
    for (;;) {
        sleep_ns(1000 * J_MS);
    }
}

static void test_own_tls(void)
{
    tls_slot = slot_new();
    uint64_t sc = slot_new();
    must(j_thread_spawn(J_ROOT_SELF, tls_slot, (uint64_t)(uintptr_t)tls_thread,
                        (uint64_t)(uintptr_t)(tls_stack + sizeof tls_stack - 8u)),
         "thread_spawn (tls)");
    must(j_sc_create(sc, 100, 10 * J_MS, 10 * J_MS), "sc_create (tls)");
    must(j_sc_bind(sc, tls_slot), "sc_bind (tls)");
    for (unsigned i = 0; i < 100u && tls_seen == 0; i++) {
        sleep_ns(J_MS);
    }
    if (tls_seen != tls_block[0]) {
        j_puts("sched: own tls: the thread saw ");
        j_putu(tls_seen);
        j_puts("\n");
    }
    check(tls_seen == tls_block[0], "a thread's own TLS base didn't take");
    must(j_thread_kill(tls_slot), "thread_kill (tls)");
    j_puts("sched: own tls ok\n");
}

/* 8. Each thread's FPU and SSE state is its own (the owner's choice,
 * 2026-11-07: eager FXSAVE, SSE only): two equals on one CPU, taking turns
 * hundreds of times, each find their own xmm0, xmm15 and MXCSR after every
 * spin, and each started from the initial state. */
static void test_fpu(void)
{
    struct worker a = worker_new(WORK_FPU, 0x0123456789abcdefull);
    struct worker b = worker_new(WORK_FPU, 0xfedcba9876543212ull);
    /* Above what test_donation leaves running (60 and 50). */
    worker_run(&a, 70, 10 * J_MS, 10 * J_MS);
    worker_run(&b, 70, 10 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS);
    worker_stop(&a);
    worker_stop(&b);
    if (a.w->aux != 0 || b.w->aux != 0 || a.w->count == 0 || b.w->count == 0) {
        j_puts("sched: fpu: counts ");
        j_putu(a.w->count);
        j_puts(" and ");
        j_putu(b.w->count);
        j_puts(", aux ");
        j_putu(a.w->aux);
        j_puts(" and ");
        j_putu(b.w->aux);
        j_puts("\n");
    }
    check(a.w->aux != 2 && b.w->aux != 2, "a new thread's FPU state wasn't the initial one");
    check(a.w->aux == 0 && b.w->aux == 0, "a thread's FPU or SSE registers changed under it");
    check(a.w->count > 0 && b.w->count > 0, "the FPU workers didn't both run");
    j_puts("sched: fpu ok\n");
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 1) {
        die("needs one module: worker");
    }
    test_init();
    occupy_other_cpus(ncpus); /* written for one CPU: lib/testroot.h */
    test_priority();
    test_budget();
    test_kernel_time();
    test_equals();
    test_donation();
    test_clock(ncpus);
    test_own_tls();
    test_fpu();
    j_puts("sched: all ok\n");
    j_exit(0);
}
