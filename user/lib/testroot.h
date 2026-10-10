/* testroot.h - what the test root tasks share (schedroot, faultroot; Phase 3
 * steps 5 and 6): workers loaded from module 0, each with a page shared with
 * the root (work.h), a way to sleep, and a check that ends the run.
 * Header only; the includer defines TEST_NAME (its prefix in the log) first. */
#ifndef JOIST_USER_TESTROOT_H
#define JOIST_USER_TESTROOT_H

#include "loader.h"
#include "work.h"

#define ROOT_WORK_VA 0x30000000ull /* the workers' pages, here, a page apart */

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
    k.task = slot_new();
    k.thread = slot_new();
    k.sc = slot_new();
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
static __attribute__((unused)) void worker_stop(struct worker *k)
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

static void check(int ok, const char *what)
{
    if (!ok) {
        j_puts(TEST_NAME ": FAILED: ");
        j_puts(what);
        j_puts("\n");
        j_exit(1);
    }
}

/* After ld_init. */
static void test_init(void)
{
    nobody = slot_new();
    must(j_obj_create(J_ENDPOINT, nobody), "obj_create");
}

#endif /* JOIST_USER_TESTROOT_H */
