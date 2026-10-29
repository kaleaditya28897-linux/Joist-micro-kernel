/* mmroot - the memory manager's test on the real kernel (Phase 4 step 6,
 * part D; decision 0023). Module 0 is mm, module 1 mmclient.
 *
 * It starts mm as decision 0023 says: its slots filled (mm_proto.h), a
 * thread with a context of its own for the first call (MM_REQ_INIT), then
 * the context taken away, so that from there on mm runs only on its
 * callers' time. Then, per client: a task loaded from module 1, handed to
 * mm (MM_REQ_ADOPT), run in one of mmclient's roles, and destroyed; after
 * each, mm must have forgotten it (no-senders, collected at its next
 * message), have every page back, and find its records sound, and the
 * kernel must have as many spaces and threads as before.
 *
 * Part D2: clients that clone (MM_REQ_CLONE) and share (MM_REQ_SHARE, with
 * the target's consent). A client's child is made by mm; the root learns of
 * it only when the parent sends it the child's task and thread capabilities
 * (MMC_RQ_START, work.h), and then loads the program's image into it, gives
 * it a work page (mapped in the parent too) and a context. */
#define TEST_NAME "mm"
#include "../lib/testroot.h"
#include "../lib/mmhost.h"

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free);

#define POOL_PAGES   HOST_POOL_PAGES
#define TABLE_RUNS   HOST_TABLE_RUNS
#define WAIT_ROUNDS  2000u
#define SHORT_NS     (5u * J_MS)

static unsigned nclients;
static struct j_counters base; /* the kernel's, with mm started */

static void expect_status(uint64_t tasks, const char *what)
{
    expect_mm(tasks, tasks == 0 ? POOL_PAGES : ANY_FREE, what);
}

/* The kernel's spaces: mm's, and `more`. */
static void expect_spaces(uint64_t more, const char *what)
{
    struct j_counters c;
    must(j_debug_counters(&c), "debug_counters");
    if (c.spaces != base.spaces + more) {
        j_puts("mm: spaces ");
        j_putu(c.spaces);
        j_puts(", wanted ");
        j_putu(base.spaces + more);
        j_puts("\n");
        check(0, what);
    }
}

/* Answer requests until the client's count reaches 2, or its aux `mark`
 * (it waits for the root to look), or time runs out. */
static void serve(struct worker *k, uint64_t mark)
{
    for (unsigned i = 0; i < WAIT_ROUNDS && k->w->count < 2 && k->w->aux != mark; i++) {
        unsigned kid;
        (void)host_serve(SHORT_NS, &kid);
    }
}

/* The client paused at `mark` (or failed): see why, if it failed. */
static void at_mark(struct worker *k, uint64_t mark, const char *what)
{
    serve(k, mark);
    if (k->w->aux != mark) {
        j_puts("mm: count ");
        j_putu(k->w->count);
        j_puts(", failed at step ");
        j_putu(k->w->arg);
        j_puts("\n");
        check(0, what);
    }
}

static void go_on(struct worker *k, uint64_t mark)
{
    k->w->aux = mark + 1u;
}

/* A client in `role`, served by mm, running. */
static struct worker client(uint64_t role)
{
    struct worker k;
    k.task = slot_new();
    k.thread = slot_new();
    k.sc = slot_new();
    uint64_t entry = ld_load(1, k.task);
    uint64_t f = page_alloc();
    uint64_t va = ROOT_WORK_VA + nworkers++ * J_PAGE;
    must(j_map(f, J_ROOT_SELF, va / J_PAGE, J_MAP_W), "map (work page, here)");
    k.w = (struct work *)(uintptr_t)va;
    k.w->role = role;
    k.w->arg = 0;
    k.w->count = 0;
    k.w->aux = 0;
    must(j_map(f, k.task, WORK_VA / J_PAGE, J_MAP_W), "map (work page, client)");

    uint64_t at = MM_S_ROOT + nclients++;
    must(j_cap_give(mm_task, k.task, at, J_R_ALL), "cap_give (client, to mm)");
    struct j_msg r = ask(MM_REQ_ADOPT, at);
    check(r.r[0] == MM_OK && r.r[1] >= 1 && r.r[1] <= MM_MAX_TASKS, "mm's ADOPT failed");
    unsigned ri = rec_new();
    must(j_cap_derive(k.task, recs[ri].task, J_R_ALL), "cap_derive (client)");
    give_root_cap(ri);
    must(j_thread_spawn(k.task, k.thread, entry, CHILD_LO), "thread_spawn (client)");
    worker_run(&k, 100, 10 * J_MS, 10 * J_MS);
    return k;
}

/* Wait until the client's count reaches `until`, or stays below it. */
static void wait_for(struct worker *k, uint64_t until)
{
    for (unsigned i = 0; i < WAIT_ROUNDS && k->w->count < until; i++) {
        sleep_ns(SHORT_NS);
    }
}

/* Destroy it, and any child of it left, finish the sweep, and see mm let
 * them go. */
static void finish(struct worker *k)
{
    must(j_task_destroy(k->task), "task_destroy (client)");
    for (unsigned i = 0; i < NRECS; i++) {
        rec_free(i);
    }
    int more = 1;
    for (unsigned i = 0; more && i < 1000u; i++) {
        must(j_continue(1000, &more), "continue");
    }
    check(!more, "the client's destruction didn't finish");
    expect_status(0, "mm didn't forget a destroyed client, or lost pages");
    expect_spaces(0, "a destroyed client's space, or its child's, is left");
}

static void test_basic(void)
{
    struct worker k = client(MMC_BASIC);
    wait_for(&k, 2);
    if (k.w->count != 2 || k.w->arg != 0) {
        j_puts("mm: basic: count ");
        j_putu(k.w->count);
        j_puts(", failed at step ");
        j_putu(k.w->arg);
        j_puts("\n");
        check(0, "the basic client failed");
    }
    expect_status(1, "mm doesn't serve the basic client");
    finish(&k);
    j_puts("mm: basic ok\n");
}

/* A fault mm refuses ends the thread: the client never gets past it. */
static void test_ended(uint64_t role, const char *name)
{
    struct worker k = client(role);
    wait_for(&k, 2);
    check(k.w->count == 1 && k.w->aux == 1 && k.w->arg == 0,
          "a fault mm refuses let the thread past it");
    /* Ended, not resumed to fault again for ever. */
    check(j_thread_suspend(k.thread) == J_E_GONE, "a fault mm refuses didn't end the thread");
    finish(&k);
    j_puts("mm: ");
    j_puts(name);
    j_puts(" ok\n");
}

/* D2: a fork. Parent and child write the same pages and each sees its
 * own; the child destroyed by its parent is forgotten and its pages come
 * back. */
static void test_fork(void)
{
    struct worker k = client(MMC_FORK);
    at_mark(&k, 1, "the fork client failed");
    /* 16 pages, 8 of them copied when written: the first writer copies,
     * the second has the last reference and keeps it. */
    expect_mm(2, POOL_PAGES - 24u, "mm's records after a fork are wrong");
    expect_spaces(2, "a clone's space isn't there");
    go_on(&k, 1);
    at_mark(&k, 3, "the fork client failed after its child went");
    expect_mm(1, POOL_PAGES - 16u, "mm didn't forget a destroyed child, or lost its pages");
    expect_spaces(1, "a destroyed child's space is left");
    go_on(&k, 3);
    serve(&k, 99);
    check(k.w->count == 2 && k.w->arg == 0, "the fork client failed at the end");
    finish(&k);
    j_puts("mm: fork ok\n");
}

/* D2: clones refused, each undone: mm's table pages spent; and the child's
 * lent pages too few for its parent's tables, mm_clone failing part way
 * (the child already has its badge). */
static void test_clone_full(void)
{
    struct worker k = client(MMC_CLONE_FULL);
    at_mark(&k, 1, "the clone-full client failed");
    /* Each index has its table run; the client's is one. */
    uint64_t n = MMC_W(k.w, MMC_RES);
    if (n != TABLE_RUNS - 1u) {
        j_puts("mm: clones before the refusal ");
        j_putu(n);
        j_puts("\n");
        check(0, "CLONE wasn't refused when mm's table pages were spent");
    }
    expect_mm(1 + n, POOL_PAGES - 4u, "a refused CLONE left a task in mm's records");
    expect_spaces(1 + n, "a refused CLONE left a space");
    go_on(&k, 1);
    at_mark(&k, 3, "the clone-full client failed");
    expect_mm(1, POOL_PAGES - 4u - MMC_FULL_REGIONS, "a CLONE refused part way left a task");
    expect_spaces(1, "a CLONE refused part way left a space");
    go_on(&k, 3);
    serve(&k, 99);
    check(k.w->count == 2 && k.w->arg == 0, "the clone-full client failed at the end");
    finish(&k);
    j_puts("mm: clones undone ok\n");
}

/* D2: SHARE needs the target's consent: refused without it, with another
 * count, or to a stale id; shared pages stay shared across a clone. */
static void test_share(void)
{
    struct worker k = client(MMC_SHARE);
    at_mark(&k, 1, "the share client failed");
    /* 8 private pages and 4 shared; one private page copied by the second
     * child's write. */
    expect_mm(3, POOL_PAGES - 13u, "mm's records with shared pages are wrong");
    expect_spaces(3, "a clone's space isn't there");
    go_on(&k, 1);
    at_mark(&k, 3, "the share client failed");
    expect_mm(1, POOL_PAGES - 16u, "mm didn't forget the children, or lost their pages");
    expect_spaces(1, "a destroyed child's space is left");
    go_on(&k, 3);
    serve(&k, 99);
    check(k.w->count == 2 && k.w->arg == 0, "the share client failed at the end");
    finish(&k);
    j_puts("mm: share ok\n");
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 2) {
        die("needs two modules: mm, mmclient");
    }
    test_init();
    host_init();
    start_mm();
    must(j_debug_counters(&base), "debug_counters");
    expect_status(0, "a fresh mm isn't empty");
    test_basic();
    test_ended(MMC_RO_WRITE, "read-only write");
    test_ended(MMC_OUTSIDE, "outside");
    test_fork();
    test_share();
    test_clone_full();
    /* Again, so that badges and table runs are used a second time. */
    test_basic();
    j_puts("mm: all ok\n");
    j_exit(0);
}
