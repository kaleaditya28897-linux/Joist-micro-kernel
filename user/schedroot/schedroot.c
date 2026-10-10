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
#include "../lib/loader.h"
#include "../lib/work.h"

#define ROOT_WORK_VA 0x30000000ull /* the workers' pages, here, a page apart */
#define SLEEP_NS     (300 * J_MS)
#define BUSY_COUNTS  30000000u
#ifndef SERVE_COUNTS
#define SERVE_COUNTS 20000u /* a request's work */
#endif

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free);

struct worker {
    uint64_t task, thread, sc;
    struct work *w;
};

static unsigned nworkers;
static uint64_t nobody; /* an endpoint to sleep on */

/* A worker task with its role, not yet running: its thread has no context. */
static struct worker worker_new(uint64_t role, uint64_t arg)
{
    struct worker k;
    k.task = next_slot++;
    k.thread = next_slot++;
    k.sc = next_slot++;
    uint64_t entry = ld_load(0, k.task);
    uint64_t f = page_alloc();
    uint64_t va = ROOT_WORK_VA + nworkers++ * J_PAGE;
    must(j_map(f, J_SLOT_SELF, va / J_PAGE, J_MAP_W), "map (work page, here)");
    k.w = (struct work *)(uintptr_t)va;
    k.w->role = role;
    k.w->arg = arg;
    k.w->count = 0;
    must(j_map(f, k.task, WORK_VA / J_PAGE, J_MAP_W), "map (work page, worker)");
    must(j_thread_spawn(k.task, k.thread, entry, CHILD_LO), "thread_spawn");
    return k;
}

/* Give it a context of its own: it can run from here on. */
static void worker_run(struct worker *k, uint64_t prio, uint64_t budget, uint64_t period)
{
    must(j_sc_create(k->sc, prio, budget, period), "sc_create");
    must(j_sc_bind(k->sc, k->thread), "sc_bind");
}

/* Take the context away again: it stops where it is. */
static void worker_stop(struct worker *k)
{
    must(j_sc_unbind(k->sc), "sc_unbind");
    must(j_cap_delete(k->sc), "cap_delete (context)");
}

static void sleep_ns(uint64_t ns)
{
    struct j_msg m = {0};
    if (j_recv(nobody, &m, ns) != J_E_TIMEOUT) {
        die("sleep: a Recv on an endpoint nobody sends to didn't time out");
    }
}

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

static void check(int ok, const char *what)
{
    if (!ok) {
        j_puts("sched: FAILED: ");
        j_puts(what);
        j_puts("\n");
        j_exit(1);
    }
}

static void report(const char *what, uint64_t pct)
{
    j_puts("sched: ");
    j_puts(what);
    j_puts(" (");
    j_putu(pct);
    j_puts("%)\n");
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
    report("budget ok: 2 ms in every 10 got", pct);
}

/* 3. Equals take turns. */
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
    report("equals ok: one of two got", pct);
}

/* 4. Donation: a passive server works on its client's time. The client
 * has 2 ms in every 10 at priority 60; the server, with no context, does
 * all the work, above a spinner at 50 and out of the client's budget. */
static void test_donation(void)
{
    struct worker server = worker_new(WORK_SERVE, SERVE_COUNTS);
    struct worker client = worker_new(WORK_CLIENT, 0);
    struct worker other = worker_new(WORK_SPIN, 0);
    uint64_t ep = next_slot++;
    must(j_obj_create(J_ENDPOINT, ep), "obj_create");
    must(j_cap_give(server.task, ep, 0, J_R_RECV), "cap_give (server)");
    must(j_cap_give(client.task, ep, 0, J_R_SEND), "cap_give (client)");
    /* The server's start: a context above the root's until its first Recv. */
    worker_run(&server, 200, 10 * J_MS, 10 * J_MS);
    worker_stop(&server);
    check(server.w->count == 0, "the server worked before anyone called");
    worker_run(&other, 50, 10 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS / 10);
    check(server.w->count == 0, "a passive server ran with no caller");
    uint64_t before = other.w->count;
    check(before != 0, "the spinner didn't run");
    worker_run(&client, 60, 2 * J_MS, 10 * J_MS);
    sleep_ns(SLEEP_NS);
    uint64_t s = server.w->count;
    uint64_t calls = client.w->count;
    uint64_t rest = other.w->count - before;
    uint64_t pct = percent(s, rest);
    check(s != 0 && calls != 0, "the server never ran on its client's time");
    check(rest != 0, "the server's work wasn't limited by its client's budget");
    check(pct >= 8 && pct <= 40, "work on donated time wasn't about the client's fifth");
    j_puts("sched: donation ok: ");
    j_putu(calls);
    j_puts(" calls served on the client's budget (");
    j_putu(pct);
    j_puts("%)\n");
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 1) {
        die("needs one module: worker");
    }
    nobody = next_slot++;
    must(j_obj_create(J_ENDPOINT, nobody), "obj_create");
    test_priority();
    test_budget();
    test_equals();
    test_donation();
    j_puts("sched: all ok\n");
    j_exit(0);
}
