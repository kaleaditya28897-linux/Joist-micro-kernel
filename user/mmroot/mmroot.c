/* mmroot - the memory manager's test on the real kernel (Phase 4 step 6,
 * part D; decision 0023). Module 0 is mm, module 1 mmclient.
 *
 * It starts mm as decision 0023 says: its slots filled (mm_proto.h), a
 * thread with a context of its own for the first call (MM_REQ_INIT), then
 * the context taken away, so that from there on mm runs only on its
 * callers' time. Then, per client: a task loaded from module 1, handed to
 * mm (MM_REQ_ADOPT), run in one of mmclient's roles, and destroyed; after
 * each, mm must have forgotten it (no-senders, collected at its next
 * message), have every page back, and find its records sound. */
#define TEST_NAME "mm"
#include "mm_proto.h"
#include "mmlib.h"
#include "../lib/testroot.h"

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free);

#define POOL_PAGES   256u
#define TABLE_PAGES  (8u * MM_TABLE_PAGES + 1u)
#define WAIT_ROUNDS  200u
#define SHORT_NS     (5u * J_MS)

static uint64_t mm_task, mm_ep, mm_root;
static unsigned nclients;

/* A frame of n pages split off the pool (loader.h's pool). */
static uint64_t frame_take(uint64_t n)
{
    while (pool_left < n) {
        if (pool_next == pool_end) {
            die("out of memory");
        }
        pool_cur = pool_next++;
        pool_left = j_frame_pages(pool_cur);
    }
    if (pool_left == n) {
        pool_left = 0;
        return pool_cur;
    }
    uint64_t rest = slot_new();
    must(j_frame_split(pool_cur, n, rest), "frame_split");
    uint64_t f = pool_cur;
    pool_cur = rest;
    pool_left -= n;
    return f;
}

/* Give mm a capability of ours in its slot dst; ours goes if `move`. */
static void give(uint64_t src, uint64_t dst, int move)
{
    must(j_cap_give(mm_task, src, dst, J_R_ALL), "cap_give (to mm)");
    if (move) {
        must(j_cap_delete(src), "cap_delete (given to mm)");
    }
}

static struct j_msg ask(uint64_t r0, uint64_t r1)
{
    struct j_msg m = {.r = {r0, r1, 0, 0}};
    must(j_call(mm_root, &m, J_FOREVER), "call (mm)");
    return m;
}

static void start_mm(void)
{
    mm_ep = slot_new();
    must(j_obj_create(J_ENDPOINT, mm_ep), "obj_create (mm's endpoint)");
    /* No-senders, bound here: mm collects the badges itself. */
    uint64_t n = slot_new();
    uint64_t nb = slot_new();
    must(j_obj_create(J_NOTIFICATION, n), "obj_create (no-senders)");
    must(j_cap_mint(n, nb, J_R_SEND, 1), "cap_mint (no-senders)");
    must(j_bind_nosenders(mm_ep, nb), "bind_nosenders");
    mm_root = slot_new();
    must(j_cap_mint(mm_ep, mm_root, J_R_SEND, MM_ROOT_BADGE), "cap_mint (the root's)");

    mm_task = slot_new();
    uint64_t entry = ld_load(0, mm_task);
    give(mm_ep, MM_S_EP, 0);
    give(mm_task, MM_S_SELF, 0);
    /* Frames mm alone holds: it maps one and splits the other. */
    give(frame_take(POOL_PAGES), MM_S_POOL, 1);
    give(frame_take(TABLE_PAGES), MM_S_TABLES, 1);

    uint64_t th = slot_new();
    uint64_t sc = slot_new();
    must(j_thread_spawn(mm_task, th, entry, CHILD_LO), "thread_spawn (mm)");
    must(j_sc_create(sc, 200, 10 * J_MS, 10 * J_MS), "sc_create (mm)");
    must(j_sc_bind(sc, th), "sc_bind (mm)");
    struct j_msg r = ask(MM_REQ_INIT, 0);
    check(r.r[0] == MM_OK, "mm's INIT failed");
    /* It answered with ReplyRecv, so it is waiting: now it is passive. */
    must(j_sc_unbind(sc), "sc_unbind (mm)");
    must(j_cap_delete(sc), "cap_delete (mm's context)");
    r = ask(MM_REQ_INIT, 0);
    check(r.r[0] == MM_E_STATE, "mm took a second INIT");
    j_puts("mm: started, passive ok\n");
}

/* mm's records: tasks served, free pages, and its own check. */
static void expect_status(uint64_t tasks, const char *what)
{
    struct j_msg r = ask(MM_REQ_STATUS, 0);
    check(r.r[0] == MM_OK && r.r[3] == 0, "mm's records are unsound");
    if (r.r[1] != tasks || (tasks == 0 && r.r[2] != POOL_PAGES)) {
        j_puts("mm: tasks ");
        j_putu(r.r[1]);
        j_puts(", free pages ");
        j_putu(r.r[2]);
        j_puts("\n");
        check(0, what);
    }
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

/* Destroy it, finish the sweep, and see mm let it go. */
static void finish(struct worker *k)
{
    must(j_task_destroy(k->task), "task_destroy (client)");
    int more = 1;
    for (unsigned i = 0; more && i < 1000u; i++) {
        must(j_continue(1000, &more), "continue");
    }
    check(!more, "the client's destruction didn't finish");
    expect_status(0, "mm didn't forget a destroyed client, or lost pages");
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

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 2) {
        die("needs two modules: mm, mmclient");
    }
    test_init();
    start_mm();
    expect_status(0, "a fresh mm isn't empty");
    test_basic();
    test_ended(MMC_RO_WRITE, "read-only write");
    test_ended(MMC_OUTSIDE, "outside");
    /* Again, so that badges and table runs are used a second time. */
    test_basic();
    j_puts("mm: all ok\n");
    j_exit(0);
}
