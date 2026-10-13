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
    j_puts("sched: all ok\n");
    j_exit(0);
}
