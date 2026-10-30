/* SPDX-License-Identifier: GPL-3.0-only */
/* faultroot.c - a root task that tests fault IPC and thread control from
 * user mode (Phase 3 step 6; docs/abi.md, *Thread control and faults*).
 * Module 0 is faulter, loaded here as a task per test; the root is its
 * fault handler: each task is given a badged send capability to the root's
 * fault endpoint in its slot HANDLER, named as its handler.
 *
 * A fault arrives as a message: kind, address, error code, and a capability
 * to the thread that faulted, through which the handler reads and writes
 * its registers before deciding, in the reply, whether it goes on.
 *
 * Faulters run at priority 100, below the root: they run while it waits. */
#define TEST_NAME "fault"
#include "../lib/testroot.h"

#define HANDLER  5u            /* the handler's slot in each faulter */
#define WAIT_NS  (500 * J_MS)  /* longer than any fault takes to arrive */
#define SHORT_NS (20 * J_MS)   /* long enough for a faulter to get as far as it can */
#define PAGER_VA 0x38000000ull /* pages the root hands out, as it sees them */
#define TLS_VA   0x60000000ull

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free);

static uint64_t fault_ep;
static uint64_t got_thread; /* where a fault's thread capability lands */
static uint64_t next_badge = 1;

/* A faulter with the root as its task's handler; its badge. */
static struct worker faulter_new(uint64_t role, uint64_t arg, uint64_t *badge)
{
    struct worker k = worker_new(role, arg);
    uint64_t tmp = slot_new();
    *badge = next_badge++;
    must(j_cap_mint(fault_ep, tmp, J_R_SEND | J_R_GRANT, *badge), "cap_mint (handler)");
    must(j_cap_give(k.task, tmp, HANDLER, J_R_SEND), "cap_give (handler)");
    must(j_cap_delete(tmp), "cap_delete");
    must(j_task_set_fault(k.task, HANDLER), "task_set_fault");
    return k;
}

static void go(struct worker *k)
{
    worker_run(k, 100, 10 * J_MS, 10 * J_MS);
}

/* Wait for a fault: its message, and the faulting thread's registers. */
static void fault_wait(struct j_msg *m, struct j_thread_regs *regs, uint64_t badge, uint64_t kind)
{
    must(j_recv_slots(got_thread, J_SLOT_NONE, 1), "recv_slots");
    *m = (struct j_msg){0};
    check(j_recv(fault_ep, m, WAIT_NS) == J_OK, "no fault arrived");
    check(m->badge == badge, "a fault from the wrong task");
    check(J_FAULT_KIND(m->r[0]) == kind, "the wrong kind of fault");
    check(J_CAPS_COUNT(m->caps) == 1 && J_CAPS_SLOT(m->caps, 0) == got_thread,
          "a fault came without its thread capability");
    must(j_thread_read_regs(got_thread, regs), "thread_read_regs");
    check(regs->pc >= CHILD_LO && regs->pc < CHILD_LO + 0x10000u, "a fault's pc isn't in the program");
    check(m->r[3] == regs->pc, "a fault message's pc isn't the thread's");
}

/* A page fault from a task whose handler asked for no thread capability
 * (J_FAULT_NO_THREAD_CAP): the message alone, the pc in it. */
static void fault_wait_bare(struct j_msg *m, uint64_t badge)
{
    *m = (struct j_msg){0};
    check(j_recv(fault_ep, m, WAIT_NS) == J_OK, "no fault arrived");
    check(m->badge == badge && J_FAULT_KIND(m->r[0]) == J_FAULT_PAGE, "not the page fault expected");
    check(J_CAPS_COUNT(m->caps) == 0, "a fault brought a thread capability it was asked not to");
    check(m->r[3] >= CHILD_LO && m->r[3] < CHILD_LO + 0x10000u, "a fault's pc isn't in the program");
}

static void fault_reply_bare(uint64_t verdict)
{
    struct j_msg m = {0};
    m.r[0] = verdict;
    must(j_reply(&m), "reply (fault)");
}

/* Answer the fault: resume, or not. */
static void fault_reply(uint64_t verdict)
{
    struct j_msg m = {0};
    m.r[0] = verdict;
    must(j_reply(&m), "reply (fault)");
    must(j_cap_delete(got_thread), "cap_delete (thread)");
}

/* 1. A fault is a message; the handler moves the thread past the bad
 * instruction and it goes on. */
static void test_resume(void)
{
    uint64_t badge;
    struct worker f = faulter_new(FAULT_UD, 0, &badge);
    struct j_msg m;
    struct j_thread_regs regs;
    go(&f);
    fault_wait(&m, &regs, badge, J_FAULT_UD);
    check(f.w->count == 1, "the faulter got past its fault unhandled");
    regs.pc += 2; /* past the ud2 */
    must(j_thread_write_regs(got_thread, &regs), "thread_write_regs");
    fault_reply(J_FAULT_RESUME);
    sleep_ns(SHORT_NS);
    check(f.w->count == 2, "a resumed thread didn't go on");
    j_puts("fault: resume ok: an invalid instruction, skipped by its handler\n");
}

/* 2. A page fault resolved by mapping the page: the root as a pager. */
static void test_pager(void)
{
    uint64_t badge;
    struct worker f = faulter_new(FAULT_PAGE, 0xfeed, &badge);
    struct j_msg m;
    struct j_thread_regs regs;
    go(&f);
    fault_wait(&m, &regs, badge, J_FAULT_PAGE);
    check(m.r[1] == FAULT_VA, "a page fault's address is wrong");
    check((m.r[2] & 7u) == 6u, "a page fault's error code isn't user, write, not present");
    check(J_FAULT_ACCESS(m.r[0]) == J_ACCESS_WRITE, "a page fault's access isn't a write, not present");
    uint64_t page = page_alloc();
    volatile uint64_t *here = (volatile uint64_t *)(uintptr_t)PAGER_VA;
    must(j_map(page, J_ROOT_SELF, PAGER_VA / J_PAGE, J_MAP_W), "map (pager, here)");
    for (unsigned i = 0; i < J_PAGE / 8u; i++) {
        here[i] = 0;
    }
    must(j_map(page, f.task, FAULT_VA / J_PAGE, J_MAP_W), "map (pager, faulter)");
    fault_reply(J_FAULT_RESUME); /* the same instruction runs again */
    sleep_ns(SHORT_NS);
    check(f.w->count == 2 && f.w->arg == 0xfeedu + 1u, "the faulter didn't go on after its page came");
    check(here[0] == 0xfeed, "the faulter's write isn't in the page it was given");
    /* And taken away again, from a task that isn't the one running: its
     * next touch must fault, not find the page through a translation the
     * CPU kept (with PCID, switching to a task no longer forgets them). */
    struct worker g = faulter_new(FAULT_PAGE2, 0, &badge);
    /* This one's handler asks for no thread capability (0007): the badge
     * names the task, and the reply alone resumes the thread. */
    must(j_task_set_fault_flags(g.task, HANDLER, J_FAULT_NO_THREAD_CAP), "task_set_fault (flags)");
    go(&g);
    fault_wait_bare(&m, badge);
    page = page_alloc();
    must(j_map(page, g.task, FAULT_VA / J_PAGE, J_MAP_W), "map (pager, second faulter)");
    fault_reply_bare(J_FAULT_RESUME);
    sleep_ns(SHORT_NS); /* it has written, and spins with the page in its TLB */
    check(g.w->count == 1, "the second faulter isn't waiting");
    must(j_unmap(g.task, FAULT_VA / J_PAGE), "unmap (from the faulter)");
    g.w->aux = 1;
    fault_wait_bare(&m, badge);
    check(m.r[1] == FAULT_VA && g.w->count == 1, "a write to an unmapped page went through");
    fault_reply_bare(1);
    j_puts("fault: pager ok: a page fault resolved by mapping the page; unmapped, it faults again\n");
}

/* 3. The handler's other verdict: the thread ends. */
static void test_terminate(void)
{
    uint64_t badge;
    struct worker f = faulter_new(FAULT_GP, 0, &badge);
    struct j_msg m;
    struct j_thread_regs regs;
    go(&f);
    fault_wait(&m, &regs, badge, J_FAULT_GP);
    fault_reply(1);
    sleep_ns(SHORT_NS);
    check(f.w->count == 1, "a terminated thread went on");
    check(j_thread_resume(f.thread) == J_E_GONE, "a terminated thread still exists");
    j_puts("fault: terminate ok: a privileged instruction, ended by its handler\n");
}

/* 4. No handler: the task is destroyed (the kernel says so in the log). */
static void test_no_handler(void)
{
    struct worker f = worker_new(FAULT_DIV, 0);
    go(&f);
    sleep_ns(SHORT_NS);
    check(f.w->count == 1, "a faulter with no handler went on");
    check(j_thread_suspend(f.thread) == J_E_GONE, "a faulter with no handler still exists");
    check(j_task_set_fault(f.task, HANDLER) == J_E_GONE, "its task still exists");
    /* A handler named but not there (an empty slot) is no handler either. */
    struct worker g = worker_new(FAULT_UD, 0);
    must(j_task_set_fault(g.task, HANDLER), "task_set_fault");
    go(&g);
    sleep_ns(SHORT_NS);
    check(g.w->count == 1, "a faulter with an empty handler slot went on");
    check(j_thread_suspend(g.thread) == J_E_GONE, "a faulter with an empty handler slot still exists");
    j_puts("fault: no handler ok: the task is gone\n");
}

/* 5. A thread's own handler comes before its task's; and a fault nobody
 * answers leaves the thread suspended, for whoever holds it to mend: the
 * root aborts the fault call, moves the pc and resumes. */
static void test_unresolved(void)
{
    uint64_t badge;
    struct worker f = faulter_new(FAULT_UD, 0, &badge);
    struct j_msg m = {0};
    struct j_thread_regs regs;
    /* Its own handler: a second capability, with another badge. */
    uint64_t tmp = slot_new();
    uint64_t own = next_badge++;
    must(j_cap_mint(fault_ep, tmp, J_R_SEND | J_R_GRANT, own), "cap_mint");
    must(j_cap_give(f.task, tmp, HANDLER + 1u, J_R_SEND), "cap_give");
    must(j_cap_delete(tmp), "cap_delete");
    must(j_thread_set_fault(f.thread, HANDLER + 1u), "thread_set_fault");
    go(&f);
    sleep_ns(SHORT_NS); /* it faults and waits in the endpoint's queue */
    check(f.w->count == 1, "the faulter got past its fault");
    must(j_thread_abort(f.thread), "thread_abort");
    check(j_recv(fault_ep, &m, 0) == J_E_TIMEOUT, "an aborted fault call is still queued");
    sleep_ns(SHORT_NS);
    check(f.w->count == 1, "a thread whose fault went unanswered ran");
    must(j_thread_read_regs(f.thread, &regs), "thread_read_regs");
    regs.pc += 2;
    must(j_thread_write_regs(f.thread, &regs), "thread_write_regs");
    must(j_thread_resume(f.thread), "thread_resume");
    sleep_ns(SHORT_NS);
    check(f.w->count == 2, "a mended thread didn't go on");
    /* And again with the fault received: the badge is the thread's own. */
    struct worker g = faulter_new(FAULT_UD, 0, &badge);
    own = next_badge++;
    tmp = slot_new();
    must(j_cap_mint(fault_ep, tmp, J_R_SEND | J_R_GRANT, own), "cap_mint");
    must(j_cap_give(g.task, tmp, HANDLER + 1u, J_R_SEND), "cap_give");
    must(j_cap_delete(tmp), "cap_delete");
    must(j_thread_set_fault(g.thread, HANDLER + 1u), "thread_set_fault");
    go(&g);
    fault_wait(&m, &regs, own, J_FAULT_UD);
    fault_reply(1);
    j_puts("fault: unresolved ok: an unanswered fault leaves the thread suspended and mendable\n");
}

/* 6. Thread control without a fault: suspend and resume, abort, and the
 * register record on a thread that hasn't run yet (its arguments, its TLS
 * base). */
static void test_control(void)
{
    struct worker s = worker_new(WORK_SPIN, 0);
    worker_run(&s, 100, 10 * J_MS, 10 * J_MS);
    sleep_ns(SHORT_NS);
    must(j_thread_suspend(s.thread), "thread_suspend");
    uint64_t c = s.w->count;
    check(c != 0, "the spinner didn't run");
    sleep_ns(SHORT_NS);
    check(s.w->count == c, "a suspended thread ran");
    must(j_thread_suspend(s.thread), "thread_suspend (twice)");
    must(j_thread_resume(s.thread), "thread_resume");
    check(j_thread_resume(s.thread) == J_E_STATE, "resuming a running thread isn't E_STATE");
    sleep_ns(SHORT_NS);
    check(s.w->count != c, "a resumed thread didn't run");
    must(j_thread_kill(s.thread), "thread_kill");
    c = s.w->count;
    sleep_ns(SHORT_NS);
    check(s.w->count == c, "a killed thread ran");

    /* Abort: a thread in a Recv that would never end gets E_ABORTED. */
    struct worker b = worker_new(FAULT_BLOCK, 0);
    uint64_t ep = slot_new();
    must(j_obj_create(J_ENDPOINT, ep), "obj_create");
    must(j_cap_give(b.task, ep, 0, J_R_RECV), "cap_give");
    go(&b);
    sleep_ns(SHORT_NS);
    check(b.w->count == 1, "the blocker isn't blocked");
    must(j_thread_abort(b.thread), "thread_abort");
    sleep_ns(SHORT_NS);
    check(b.w->count == 2 && b.w->arg == J_E_ABORTED, "an aborted Recv didn't return E_ABORTED");

    /* Registers written before a thread first runs are what it starts with. */
    struct worker r = worker_new(FAULT_REGS, 0);
    struct j_thread_regs regs;
    must(j_thread_read_regs(r.thread, &regs), "thread_read_regs");
    check(regs.sp == CHILD_LO && regs.r[J_RDI] == 0, "a new thread's record isn't what spawn set");
    regs.r[J_RDI] = 0x1234;
    regs.r[J_RSI] = 0x5678;
    must(j_thread_write_regs(r.thread, &regs), "thread_write_regs");
    go(&r);
    sleep_ns(SHORT_NS);
    check(r.w->count == 2 && r.w->arg == (0x1234ull << 32 | 0x5678u), "written registers didn't arrive");

    /* The TLS base is a field of the record (0010). */
    struct worker t = worker_new(FAULT_TLS, 0);
    uint64_t page = page_alloc();
    volatile uint64_t *here = (volatile uint64_t *)(uintptr_t)(PAGER_VA + J_PAGE);
    must(j_map(page, J_ROOT_SELF, (PAGER_VA + J_PAGE) / J_PAGE, J_MAP_W), "map (tls, here)");
    here[0] = 0x715;
    must(j_map(page, t.task, TLS_VA / J_PAGE, 0), "map (tls, thread)");
    must(j_thread_read_regs(t.thread, &regs), "thread_read_regs");
    regs.tls = TLS_VA;
    must(j_thread_write_regs(t.thread, &regs), "thread_write_regs");
    go(&t);
    sleep_ns(SHORT_NS);
    check(t.w->count == 2 && t.w->arg == 0x715, "the TLS base didn't arrive");

    /* What the kernel must not trust. A kernel address as the TLS base and
     * flags with privileges: the thread runs, without them. */
    struct worker e = worker_new(FAULT_REGS, 0);
    must(j_thread_read_regs(e.thread, &regs), "thread_read_regs");
    regs.tls = 0xffffffff80000000ull;
    regs.flags = 0x3000u | 0x200u | 0x100u; /* IOPL 3, and the trap flag */
    must(j_thread_write_regs(e.thread, &regs), "thread_write_regs");
    go(&e);
    sleep_ns(SHORT_NS);
    check(e.w->count == 2, "a thread given bad flags and TLS base didn't run (sanitised)");

    /* A pc in the kernel half: the thread never runs there; it is a fault
     * of its own (general protection), which its handler sees like any other. */
    uint64_t badge;
    struct worker p = faulter_new(FAULT_REGS, 0, &badge);
    struct j_msg m = {0};
    must(j_thread_read_regs(p.thread, &regs), "thread_read_regs");
    regs.pc = 0xffffffff80000000ull;
    must(j_thread_write_regs(p.thread, &regs), "thread_write_regs");
    go(&p);
    must(j_recv_slots(got_thread, J_SLOT_NONE, 1), "recv_slots");
    check(j_recv(fault_ep, &m, WAIT_NS) == J_OK && m.badge == badge && m.r[0] == J_FAULT_GP,
          "a thread with a kernel pc didn't fault");
    must(j_thread_read_regs(got_thread, &regs), "thread_read_regs");
    check(regs.pc == 0xffffffff80000000ull && p.w->count == 0, "a thread with a kernel pc ran");
    fault_reply(1);
    j_puts("fault: control ok: suspend, resume, kill, abort, registers, TLS base, untrusted values\n");
}

/* 7. The fault round trip, measured (Phase 4 step 3, 0007): a faulter
 * touches fresh pages one after another; this handler does what mm will for
 * each (receive the fault, map a page there, reply), and, if it was given
 * one, deletes the thread capability. The faulter times each round trip. */
#ifndef BENCH_FAULTS
#define BENCH_FAULTS FAULT_BENCH_MAX
#endif
/* This handler's stamps, per fault: its Recv returned (tr), its map
 * returned (tm), its reply returned (tp). With the faulter's t0 and t1
 * they split each round trip in four (bench_breakdown). */
static uint64_t tr[BENCH_FAULTS], tm[BENCH_FAULTS], tp[BENCH_FAULTS];

static uint64_t median_of(uint64_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; i++) { /* insertion sort: n is small */
        uint64_t x = v[i];
        unsigned j = i;
        for (; j > 0 && v[j - 1u] > x; j--) {
            v[j] = v[j - 1u];
        }
        v[j] = x;
    }
    return n != 0 ? v[n / 2u] : 0;
}

/* The median of each part of the round trip: to the handler (the fault,
 * delivered, the handler running: t0 to tr), its map (tr to tm), its reply
 * (tm to tp), and back (its capability delete if any, its next Recv, the
 * faulter running again: tp to t1). */
static void bench_breakdown(const struct work *w, const char *how)
{
    static uint64_t part[BENCH_FAULTS];
    const volatile uint64_t *t0 = FAULT_BENCH_T0(w);
    const volatile uint64_t *t1 = FAULT_BENCH_T1(w);
    j_puts("fault: bench: ");
    j_puts(how);
    j_puts(": to the handler ");
    for (unsigned i = 0; i < BENCH_FAULTS; i++) {
        part[i] = tr[i] - t0[i];
    }
    j_putu(median_of(part, BENCH_FAULTS));
    j_puts(", map ");
    for (unsigned i = 0; i < BENCH_FAULTS; i++) {
        part[i] = tm[i] - tr[i];
    }
    j_putu(median_of(part, BENCH_FAULTS));
    j_puts(", reply ");
    for (unsigned i = 0; i < BENCH_FAULTS; i++) {
        part[i] = tp[i] - tm[i];
    }
    j_putu(median_of(part, BENCH_FAULTS));
    j_puts(", back ");
    for (unsigned i = 0; i < BENCH_FAULTS; i++) {
        part[i] = t1[i] - tp[i];
    }
    j_putu(median_of(part, BENCH_FAULTS));
    j_puts(" cycles (medians)\n");
}

static uint64_t bench_run(int bare, uint64_t *fastest)
{
    uint64_t badge;
    struct worker f = faulter_new(FAULT_BENCH, BENCH_FAULTS, &badge);
    if (bare) {
        must(j_task_set_fault_flags(f.task, HANDLER, J_FAULT_NO_THREAD_CAP),
             "task_set_fault (flags)");
    }
    uint64_t page = page_alloc();
    go(&f);
    for (unsigned i = 0; i < BENCH_FAULTS; i++) {
        struct j_msg m = {0};
        if (!bare) {
            must(j_recv_slots(got_thread, J_SLOT_NONE, 1), "recv_slots");
        }
        uint64_t e = j_recv(fault_ep, &m, WAIT_NS);
        tr[i] = work_tsc();
        check(e == J_OK && m.badge == badge &&
                  J_FAULT_KIND(m.r[0]) == J_FAULT_PAGE && J_FAULT_ACCESS(m.r[0]) == J_ACCESS_WRITE &&
                  m.r[1] == FAULT_BENCH_VA + i * J_PAGE,
              "the benchmark's fault isn't the next page written");
        check(J_CAPS_COUNT(m.caps) == (bare ? 0u : 1u),
              "the benchmark's fault has the wrong capabilities");
        must(j_map(page, f.task, m.r[1] / J_PAGE, J_MAP_W), "map (benchmark)");
        tm[i] = work_tsc();
        struct j_msg r = {0};
        r.r[0] = J_FAULT_RESUME;
        must(j_reply(&r), "reply (benchmark)");
        tp[i] = work_tsc();
        if (!bare) {
            must(j_cap_delete(got_thread), "cap_delete (thread)");
        }
    }
    for (unsigned i = 0; i < 50u && f.w->count != 2; i++) {
        sleep_ns(SHORT_NS);
    }
    check(f.w->count == 2, "the benchmark's faulter didn't finish");
    must(j_unmap_range(f.task, FAULT_BENCH_VA / J_PAGE, BENCH_FAULTS), "unmap_range (benchmark)");
    bench_breakdown(f.w, bare ? "without a thread capability" : "with a thread capability");
    *fastest = f.w->arg;
    return f.w->aux;
}

/* The same with a passive handler (Phase 4 step 3: is the cost the change
 * of scheduling context?): a thread of this task with no context of its
 * own, running only on the time each fault lends it (0009), answering and
 * waiting in one call (ReplyRecv: after a plain reply it would have no time
 * left to wait again). No thread capability; its reply and next wait are
 * one call, so "reply" reads 0 and "back" holds both. */
static uint64_t ph_page, ph_task;
static volatile unsigned ph_n;
static uint8_t ph_stack[16384] __attribute__((aligned(16)));

static void passive_handler(void)
{
    struct j_msg m = {0};
    if (j_recv(fault_ep, &m, J_FOREVER) != J_OK) { /* the start-up handshake */
        die("the passive handler's first Recv failed");
    }
    for (unsigned i = 0;; i++) {
        m = (struct j_msg){0};
        m.r[0] = J_FAULT_RESUME;
        uint64_t e = j_reply_recv(fault_ep, &m, J_FOREVER);
        uint64_t now = work_tsc();
        if (i >= BENCH_FAULTS) {
            die("the passive handler got more faults than the benchmark makes");
        }
        tr[i] = now;
        if (e != J_OK || J_FAULT_KIND(m.r[0]) != J_FAULT_PAGE ||
            m.r[1] != FAULT_BENCH_VA + i * J_PAGE) {
            die("the passive handler's fault isn't the next page written");
        }
        if (j_map(ph_page, ph_task, m.r[1] / J_PAGE, J_MAP_W) != J_OK) {
            die("the passive handler's map failed");
        }
        tm[i] = tp[i] = work_tsc();
        ph_n = i + 1u;
    }
}

static uint64_t bench_passive(uint64_t *fastest)
{
    uint64_t badge;
    struct worker f = faulter_new(FAULT_BENCH, BENCH_FAULTS, &badge);
    must(j_task_set_fault_flags(f.task, HANDLER, J_FAULT_NO_THREAD_CAP),
         "task_set_fault (flags)");
    ph_page = page_alloc();
    ph_task = f.task;
    uint64_t th = slot_new();
    uint64_t sc = slot_new();
    must(j_thread_spawn(J_ROOT_SELF, th, (uint64_t)(uintptr_t)passive_handler,
                        (uint64_t)(uintptr_t)(ph_stack + sizeof ph_stack - 8u)),
         "thread_spawn (passive handler)");
    /* It reaches its Recv on a context of its own, answers one call (so it
     * is waiting before the context goes, docs/abi.md), and gives it up. */
    must(j_sc_create(sc, 200, 10 * J_MS, 10 * J_MS), "sc_create (passive handler)");
    must(j_sc_bind(sc, th), "sc_bind (passive handler)");
    struct j_msg h = {0};
    must(j_call(fault_ep, &h, J_FOREVER), "call (passive handler start)");
    must(j_sc_unbind(sc), "sc_unbind (passive handler)");
    go(&f);
    for (unsigned i = 0; i < 50u && f.w->count != 2; i++) {
        sleep_ns(SHORT_NS);
    }
    check(f.w->count == 2 && ph_n == BENCH_FAULTS, "the passive handler's benchmark didn't finish");
    must(j_unmap_range(f.task, FAULT_BENCH_VA / J_PAGE, BENCH_FAULTS), "unmap_range (benchmark)");
    bench_breakdown(f.w, "a passive handler, without a thread capability");
    *fastest = f.w->arg;
    return f.w->aux;
}

static void test_bench(void)
{
    uint64_t fast_cap;
    uint64_t fast_bare;
    uint64_t med_cap = bench_run(0, &fast_cap);
    uint64_t med_bare = bench_run(1, &fast_bare);
    j_puts("fault: bench: a page fault, mapped and resumed: median ");
    j_putu(med_cap);
    j_puts(" cycles with a thread capability (fastest ");
    j_putu(fast_cap);
    j_puts("), ");
    j_putu(med_bare);
    j_puts(" without (fastest ");
    j_putu(fast_bare);
    j_puts(")\n");
    uint64_t fast_passive;
    uint64_t med_passive = bench_passive(&fast_passive);
    j_puts("fault: bench: the same with a passive handler: median ");
    j_putu(med_passive);
    j_puts(" cycles (fastest ");
    j_putu(fast_passive);
    j_puts(")\n");
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 1) {
        die("needs one module: faulter");
    }
    test_init();
    fault_ep = slot_new();
    got_thread = slot_new();
    must(j_obj_create(J_ENDPOINT, fault_ep), "obj_create");
    test_resume();
    test_pager();
    test_terminate();
    test_no_handler();
    test_unresolved();
    test_control();
    test_bench();
    j_puts("fault: all ok\n");
    j_exit(0);
}
