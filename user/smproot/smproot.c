/* smproot.c - a root task that tests the kernel on more than one CPU (Phase
 * 3 step 9; decision 0016). Module 0 is worker. The kernel tells the root
 * how many CPUs it uses; with one, this has nothing to test and says so.
 *
 * As in the scheduler test, each worker shares a page with the root and
 * its count moves only while it runs; here several move at once.
 *
 * 1. CPUs run threads at the same time: as many spinners as CPUs each get
 *    about what one gets alone.
 * 2. Priorities hold across CPUs: with one thread more than CPUs, the
 *    lowest doesn't run; and the root, waking, takes the CPU of the lowest
 *    that does, not anyone else's.
 * 3. IPC between CPUs: a client and a server with contexts of their own
 *    keep answering each other, each waking the other across CPUs.
 * 4. A thread running on another CPU can be stopped, read, resumed and
 *    killed from here, and it takes effect at once.
 * 5. Tasks whose threads are running on other CPUs can be destroyed.
 *
 * Times are generous: an emulator gives each CPU a thread of its own, on a
 * host that may have fewer to spare. */
#define TEST_NAME "smp"
#include "../lib/testroot.h"

#define SLEEP_NS (300 * J_MS)
#define SHORT_NS (30 * J_MS)
#define MAX_CPUS 8u

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus);

static void put(const char *what, uint64_t v)
{
    j_puts(what);
    j_putu(v);
}

/* Be busy here, without calling the kernel, for about `ns` (by the time
 * stamp counter, `per_ms` counts to the millisecond). */
static void busy_for(uint64_t ns, uint64_t per_ms)
{
    uint64_t until = work_tsc() + ns / 1000u * per_ms / 1000u;
    while (work_tsc() < until) {
    }
}

/* Does the worker's count move within `ns`? (Busy here meanwhile; it
 * returns as soon as it has. A wait, not a fixed window: an emulated CPU
 * can lose its host thread for longer than a short window lasts.) */
static int moves(const struct worker *k, uint64_t ns, uint64_t per_ms)
{
    uint64_t c = k->w->count;
    uint64_t until = work_tsc() + ns / 1000u * per_ms / 1000u;
    while (work_tsc() < until) {
        if (k->w->count != c) {
            return 1;
        }
    }
    return 0;
}

static void finish(struct worker *k)
{
    int more = 1;
    must(j_task_destroy(k->task), "task_destroy");
    must(j_cap_delete(k->task), "cap_delete");
    while (more) {
        must(j_continue(0, &more), "continue");
    }
}

/* 1. */
static void test_parallel(uint64_t ncpus)
{
    struct worker one = worker_new(WORK_SPIN, 0);
    worker_run(&one, 100, 10 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS);
    worker_stop(&one);
    uint64_t alone = one.w->count;
    check(alone != 0, "a spinner didn't run");

    struct worker s[MAX_CPUS];
    for (uint64_t i = 0; i < ncpus; i++) {
        s[i] = worker_new(WORK_SPIN, 0);
    }
    for (uint64_t i = 0; i < ncpus; i++) {
        worker_run(&s[i], 100, 10 * J_MS, 10 * J_MS);
    }
    sleep_ns(SLEEP_NS);
    uint64_t least = UINT64_MAX;
    uint64_t sum = 0;
    for (uint64_t i = 0; i < ncpus; i++) {
        worker_stop(&s[i]);
    }
    for (uint64_t i = 0; i < ncpus; i++) {
        least = s[i].w->count < least ? s[i].w->count : least;
        sum += s[i].w->count;
    }
    /* On one CPU each would get alone / ncpus. Half of what one gets alone
     * is out of reach without running at the same time, on 2 CPUs or 8. */
    check(least * 2u >= alone, "spinners on separate CPUs didn't run at the same time");
    put("smp: parallel ok: ", ncpus);
    put(" spinners together did ", sum * 100u / alone);
    j_puts("% of what one does alone\n");
    finish(&one);
    for (uint64_t i = 0; i < ncpus; i++) {
        finish(&s[i]);
    }
}

/* 2. */
static void test_priorities(uint64_t ncpus, uint64_t per_ms)
{
    struct worker s[MAX_CPUS];
    struct worker low = worker_new(WORK_SPIN, 0);
    worker_run(&low, 50, 10 * J_MS, 10 * J_MS);
    for (uint64_t i = 0; i < ncpus; i++) {
        s[i] = worker_new(WORK_SPIN, 0);
        worker_run(&s[i], 90 + i, 10 * J_MS, 10 * J_MS); /* s[0] the lowest of them */
    }
    /* Asleep: the ncpus spinners have the CPUs, the one below them none.
     * (It ran until they were all started; from here it mustn't.) */
    sleep_ns(SHORT_NS);
    uint64_t l0 = low.w->count;
    sleep_ns(SLEEP_NS);
    check(low.w->count == l0, "a thread ran though every CPU had a higher priority to run");
    for (uint64_t i = 0; i < ncpus; i++) {
        check(s[i].w->count != 0, "a spinner got no CPU");
    }
    /* Awake and busy: the root (128) is above them all, and takes a CPU.
     * It must be s[0]'s, the lowest; the others go on. */
    uint64_t before[MAX_CPUS];
    busy_for(20 * J_MS, per_ms); /* (let the change of places settle) */
    for (uint64_t i = 0; i < ncpus; i++) {
        before[i] = s[i].w->count;
    }
    busy_for(200 * J_MS, per_ms);
    uint64_t lowest = s[0].w->count - before[0];
    for (uint64_t i = 1; i < ncpus; i++) {
        uint64_t d = s[i].w->count - before[i];
        check(d != 0, "a higher priority lost its CPU to the root");
        check(lowest * 10u < d, "the root didn't take the lowest priority's CPU");
    }
    check(low.w->count == l0, "the lowest of all ran");
    j_puts("smp: priorities ok: the lowest waits; a waking thread takes the lowest's CPU\n");
    finish(&low);
    for (uint64_t i = 0; i < ncpus; i++) {
        finish(&s[i]);
    }
}

/* 3. */
static void test_ipc(void)
{
    struct worker server = worker_new(WORK_SERVE, 2000);
    struct worker client = worker_new(WORK_CLIENT, 0);
    uint64_t ep = slot_new();
    must(j_obj_create(J_ENDPOINT, ep), "obj_create");
    must(j_cap_give(server.task, ep, 0, J_R_RECV), "cap_give (server)");
    must(j_cap_give(client.task, ep, 0, J_R_SEND), "cap_give (client)");
    /* Each with a context of its own, equals: nothing is lent, and with a
     * CPU each, every call and every reply wakes a thread on the other. */
    worker_run(&server, 100, 10 * J_MS, 10 * J_MS);
    worker_run(&client, 100, 10 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS);
    uint64_t calls = client.w->count;
    check(calls >= 100, "a client and a server on two CPUs hardly spoke");
    check(server.w->count >= calls * 2000u, "the server's count doesn't match the calls answered");
    put("smp: ipc ok: ", calls);
    j_puts(" calls answered between two CPUs\n");
    finish(&client);
    finish(&server);
    must(j_cap_delete(ep), "cap_delete");
}

/* 4. */
static void test_control(uint64_t per_ms)
{
    struct worker s = worker_new(WORK_SPIN, 0);
    struct j_thread_regs regs;
    worker_run(&s, 100, 10 * J_MS, 10 * J_MS);
    busy_for(20 * J_MS, per_ms); /* the root is busy here; the spinner runs elsewhere */
    uint64_t c;
    check(moves(&s, 200 * J_MS, per_ms), "the spinner isn't running beside the root");
    for (unsigned round = 0; round < 50u; round++) {
        /* Stopped from here while it runs there: it has stopped by the
         * time the call returns, not a moment later. */
        must(j_thread_suspend(s.thread), "thread_suspend");
        c = s.w->count;
        busy_for(1 * J_MS, per_ms);
        check(s.w->count == c, "a thread suspended from another CPU went on running");
        must(j_thread_read_regs(s.thread, &regs), "thread_read_regs");
        check(regs.pc >= CHILD_LO && regs.pc < CHILD_LO + 0x10000u, "its registers aren't its own");
        must(j_thread_resume(s.thread), "thread_resume");
        check(moves(&s, 200 * J_MS, per_ms), "a thread resumed from another CPU didn't run");
        /* Read while it runs: it is stopped for the reading and goes on. */
        must(j_thread_read_regs(s.thread, &regs), "thread_read_regs (running)");
        check(regs.pc >= CHILD_LO && regs.pc < CHILD_LO + 0x10000u, "a running thread's registers");
        check(moves(&s, 200 * J_MS, per_ms), "a thread didn't go on after its registers were read");
    }
    must(j_thread_kill(s.thread), "thread_kill");
    c = s.w->count;
    busy_for(5 * J_MS, per_ms);
    check(s.w->count == c, "a thread killed from another CPU went on running");
    j_puts("smp: control ok: a thread on another CPU stopped, read, resumed 50 times, and killed\n");
    finish(&s);
}

/* 5. */
static void test_destroy(uint64_t ncpus, uint64_t per_ms)
{
    for (unsigned round = 0; round < 20u; round++) {
        struct worker s[MAX_CPUS];
        for (uint64_t i = 0; i + 1u < ncpus; i++) {
            s[i] = worker_new(WORK_SPIN, 0);
            worker_run(&s[i], 100, 10 * J_MS, 10 * J_MS);
        }
        for (uint64_t i = 0; i + 1u < ncpus; i++) {
            /* It is running, in its own address space, on another CPU. */
            check(moves(&s[i], 200 * J_MS, per_ms), "a spinner isn't running beside the root");
            uint64_t sc = s[i].sc;
            finish(&s[i]); /* its thread and its address space go while it runs */
            must(j_cap_delete(sc), "cap_delete (context)");
            uint64_t c = s[i].w->count;
            busy_for(J_MS / 5u, per_ms);
            check(s[i].w->count == c, "a destroyed task's thread went on running");
        }
    }
    j_puts("smp: destroy ok: 20 rounds of tasks destroyed while their threads ran on other CPUs\n");
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 1) {
        die("needs one module: worker");
    }
    test_init();
    put("smp: ", ncpus);
    j_puts(" CPUs\n");
    if (ncpus < 2) {
        j_puts("smp: one CPU: nothing to test\n");
        j_exit(0);
    }
    check(ncpus <= MAX_CPUS, "more CPUs than this test has room for");
    uint64_t t0 = work_tsc();
    sleep_ns(50 * J_MS);
    uint64_t per_ms = (work_tsc() - t0) / 50u;
    check(per_ms != 0, "the time stamp counter doesn't advance");
    test_parallel(ncpus);
    test_priorities(ncpus, per_ms);
    test_ipc();
    test_control(per_ms);
    test_destroy(ncpus, per_ms);
    j_puts("smp: all ok\n");
    j_exit(0);
}
