/* SPDX-License-Identifier: GPL-3.0-only */
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
 * it a work page (mapped in the parent too) and a context. The parent then
 * confirms the fork (MM_REQ_CLONE_DONE); a child still pending when its
 * parent goes is destroyed with it (the orphans test). */
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
static uint64_t last_badge; /* the last client's, from ADOPT */
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
/* A client in `role`, served by mm, running; its tree unlimited (0028) if
 * `unlimited` (for a test of mm being full, not of a tree's limits). */
/* An exception handler for the next client, named before its thread
 * starts: an endpoint (0 none) and the badge its capability carries. */
static uint64_t next_handler_ep, next_handler_badge;
#define PASS_SLOT 14u /* the client's slot for it (not one mmclient uses) */

static struct worker client_tree(uint64_t role, int unlimited)
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
    last_badge = r.r[1];
    if (unlimited) {
        tree_unlimited(r.r[1]);
    }
    unsigned ri = rec_new();
    must(j_cap_derive(k.task, recs[ri].task, J_R_ALL), "cap_derive (client)");
    give_root_cap(ri);
    if (next_handler_ep != 0) {
        uint64_t c = slot_new();
        must(j_cap_mint(next_handler_ep, c, J_R_SEND | J_R_GRANT, next_handler_badge),
             "cap_mint (a client's exception handler)");
        must(j_cap_give(k.task, c, PASS_SLOT, J_R_SEND), "cap_give (a client's exception handler)");
        must(j_cap_delete(c), "cap_delete (a client's exception handler)");
        must(j_task_set_fault(k.task, PASS_SLOT), "task_set_fault (a client's exception handler)");
        next_handler_ep = 0;
    }
    must(j_thread_spawn(k.task, k.thread, entry, CHILD_LO), "thread_spawn (client)");
    worker_run(&k, 100, 10 * J_MS, 10 * J_MS);
    return k;
}

static struct worker client(uint64_t role)
{
    return client_tree(role, 0);
}

/* Wait until the client's count reaches `until`, or stays below it. */
static void wait_for(struct worker *k, uint64_t until)
{
    for (unsigned i = 0; i < WAIT_ROUNDS && k->w->count < until; i++) {
        sleep_ns(SHORT_NS);
    }
}

static void sweep(void)
{
    int more = 1;
    for (unsigned i = 0; more && i < 1000u; i++) {
        must(j_continue(1000, &more), "continue");
    }
    check(!more, "a destruction didn't finish");
}

/* Every record's task let go (a child destroyed), and mm must let them go. */
static void finish_all(void)
{
    for (unsigned i = 0; i < NRECS; i++) {
        rec_free(i);
    }
    sweep();
    expect_status(0, "mm didn't forget a destroyed client, or lost pages");
    expect_spaces(0, "a destroyed client's space, or its child's, is left");
}

/* Destroy it, and any child of it left, finish the sweep, and see mm let
 * them go. */
static void finish(struct worker *k)
{
    must(j_task_destroy(k->task), "task_destroy (client)");
    finish_all();
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

/* A fault mm refuses is passed on (0027); with no exception handler the
 * kernel ends the task: the client never gets past it, and mm forgets it. */
static void test_ended(uint64_t role, const char *name)
{
    struct worker k = client(role);
    wait_for(&k, 2);
    check(k.w->count == 1 && k.w->aux == 1 && k.w->arg == 0,
          "a fault mm refuses let the thread past it");
    /* Ended, not resumed to fault again for ever: the task, not the thread. */
    check(j_thread_suspend(k.thread) == J_E_GONE, "a fault mm refuses didn't end the thread");
    check(j_task_destroy(k.task) == J_E_GONE, "a fault mm passed on with no handler didn't end the task");
    finish_all();
    j_puts("mm: ");
    j_puts(name);
    j_puts(" ok\n");
}

/* Phase 6 step 3 (0027): an exception handler of the test's own, an
 * endpoint here that the client's task names (task_set_fault, with a thread
 * capability: glibc's signal thread will be named so). */
static uint64_t pass_ep, pass_thread;

static uint64_t name_handler(struct worker *k, uint64_t badge)
{
    uint64_t c = slot_new();
    must(j_cap_mint(pass_ep, c, J_R_SEND | J_R_GRANT, badge), "cap_mint (exception handler)");
    must(j_cap_give(k->task, c, PASS_SLOT, J_R_SEND), "cap_give (exception handler)");
    must(j_cap_delete(c), "cap_delete (exception handler)");
    return j_task_set_fault(k->task, PASS_SLOT);
}

/* Bad accesses mm passes on reach the handler (here) with the code, the
 * address, the access and the pc; it writes the thread's registers (past
 * the access, RAX set) and resumes it. Faults mm serves never come here. */
static void test_passed(void)
{
    struct worker k = client(MMC_PASSED);
    at_mark(&k, 1, "the passed-faults client failed");
    must(name_handler(&k, 0x9a55u), "task_set_fault (the client's exception handler)");
    pass_thread = slot_new();
    go_on(&k, 1);
    static const struct {
        uint64_t va, access, code, at;
    } want[3] = {
        {MMC_VA + 16u * J_PAGE, J_ACCESS_READ, J_SEGV_MAPERR, MMC_P_LOAD_AT},
        {MMC_VA, J_ACCESS_WRITE | J_ACCESS_PRESENT, J_SEGV_ACCERR, MMC_P_STORE_AT},
        {MMC_VA + 4u * J_PAGE, J_ACCESS_READ, J_SEGV_ACCERR, MMC_P_LOAD_AT},
    };
    for (unsigned i = 0; i < 3u; i++) {
        struct j_msg m = {0};
        must(j_recv_slots(pass_thread, J_SLOT_NONE, 1), "recv_slots");
        check(j_recv(pass_ep, &m, 2000u * J_MS) == J_OK, "a fault mm passed on didn't arrive");
        if (m.badge != 0x9a55u || J_FAULT_KIND(m.r[0]) != J_FAULT_PAGE || (m.r[0] & J_FAULT_PASSED) == 0 ||
            J_FAULT_CODE(m.r[0]) != want[i].code || m.r[1] != want[i].va ||
            J_FAULT_ACCESS(m.r[0]) != want[i].access || m.r[3] != MMC_W(k.w, want[i].at)) {
            j_puts("mm: passed fault ");
            j_putu(i + 1u);
            j_puts(": r0 ");
            j_putu(m.r[0]);
            j_puts(", address ");
            j_putu(m.r[1]);
            j_puts(", pc ");
            j_putu(m.r[3]);
            j_puts("\n");
            check(0, "a passed fault's kind, mark, code, address, access or pc is wrong");
        }
        check(J_CAPS_COUNT(m.caps) == 1 && J_CAPS_SLOT(m.caps, 0) == pass_thread,
              "a passed fault came without its thread capability");
        struct j_thread_regs regs;
        must(j_thread_read_regs(pass_thread, &regs), "thread_read_regs");
        check(regs.pc == m.r[3], "a passed fault's pc isn't the thread's");
        regs.pc += MMC_PASS_INSN;
        regs.r[J_RAX] = MMC_PASS_RAX(i + 1u);
        must(j_thread_write_regs(pass_thread, &regs), "thread_write_regs");
        m = (struct j_msg){.r = {J_FAULT_RESUME}};
        must(j_reply(&m), "reply (passed fault)");
        must(j_cap_delete(pass_thread), "cap_delete (thread)");
    }
    wait_for(&k, 2);
    check(k.w->count == 2 && k.w->arg == 0 && MMC_W(k.w, MMC_P_N) == 3,
          "the client didn't go on past its passed faults as its handler said");
    struct j_msg none = {0};
    check(j_recv(pass_ep, &none, 0) == J_E_TIMEOUT, "a fault mm serves reached the exception handler");
    must(j_recv_slots(rx0, rx1, 2), "recv_slots"); /* back where a child's capabilities come (mmhost.h) */
    finish(&k);
    j_puts("mm: passed ok\n");
}

/* A handler that takes the fault and never answers: it keeps the reply
 * (reply_save) and waits on an endpoint nobody calls. */
static uint64_t hung_robj, hung_park;
static volatile uint64_t hung_got;
static uint8_t hung_stack[8192] __attribute__((aligned(16)));

static void hung_handler(void)
{
    struct j_msg m = {0};
    if (j_recv(pass_ep, &m, J_FOREVER) == J_OK && j_reply_save(hung_robj) == J_OK) {
        hung_got = m.r[0];
    }
    for (;;) {
        (void)j_recv(hung_park, &m, J_FOREVER);
    }
}

/* The client's thread waits in its fault call for ever, and mm, which
 * passed the fault on and went back to its loop, serves other tasks'
 * faults all the while: a fork's demand-zero and copy-on-write faults (in
 * the parent and its child), a basic client's map, protect and unmap.
 * Then the client goes, and mm forgets it. */
static void test_hung(void)
{
    hung_robj = slot_new();
    hung_park = slot_new();
    must(j_obj_create(J_REPLY, hung_robj), "obj_create (reply)");
    must(j_obj_create(J_ENDPOINT, hung_park), "obj_create (park)");
    uint64_t th = slot_new();
    uint64_t sc = slot_new();
    must(j_thread_spawn(J_ROOT_SELF, th, (uint64_t)(uintptr_t)hung_handler,
                        (uint64_t)(uintptr_t)(hung_stack + sizeof hung_stack - 8u)),
         "thread_spawn (hung handler)");
    must(j_sc_create(sc, 150, 2 * J_MS, 10 * J_MS), "sc_create (hung handler)");
    must(j_sc_bind(sc, th), "sc_bind (hung handler)");
    struct worker k = client(MMC_PASSED);
    at_mark(&k, 1, "the hung handler's client failed");
    must(name_handler(&k, 0x4a46u), "task_set_fault (the hung handler)");
    go_on(&k, 1);
    for (unsigned i = 0; i < WAIT_ROUNDS && hung_got == 0; i++) {
        sleep_ns(SHORT_NS);
    }
    check(hung_got != 0 && (hung_got & J_FAULT_PASSED) != 0 && J_FAULT_CODE(hung_got) == J_SEGV_MAPERR,
          "the hung handler didn't get the passed fault");
    /* mm serves on: a fork (demand-zero, then copy-on-write in both),
     * then a basic client, each fault through mm. */
    struct worker f = client(MMC_FORK);
    at_mark(&f, 1, "a fork beside a hung handler failed");
    go_on(&f, 1);
    at_mark(&f, 3, "a fork beside a hung handler failed after its child went");
    go_on(&f, 3);
    serve(&f, 99);
    check(f.w->count == 2 && f.w->arg == 0, "a fork beside a hung handler failed at the end");
    must(j_task_destroy(f.task), "task_destroy (fork)");
    /* (A basic client, not a steady one: each client costs the root its
     * image's pages for good, and the trees test after needs them.) */
    struct worker s = client(MMC_BASIC);
    wait_for(&s, 2);
    check(s.w->count == 2 && s.w->arg == 0, "a basic client beside a hung handler failed");
    must(j_task_destroy(s.task), "task_destroy (basic)");
    /* The hung one is still where it was, in its fault call, and mm,
     * once the others are swept, serves it alone. */
    check(k.w->count == 1 && k.w->arg == 0 && MMC_W(k.w, MMC_P_N) == 0,
          "the client of a hung handler got past its fault");
    sweep();
    expect_status(1, "mm didn't forget the clients beside a hung handler, or lost the hung one");
    finish(&k); /* the saved reply is stale now; mm forgets the task */
    struct j_msg late = {.r = {J_FAULT_RESUME}};
    check(j_reply_saved(hung_robj, &late) == J_E_PEER, "a hung handler's late answer reached someone");
    must(j_thread_kill(th), "thread_kill (hung handler)");
    j_puts("mm: hung handler ok: mm served a fork and a basic client while a handler held a passed fault\n");
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
    struct worker k = client_tree(MMC_CLONE_FULL, 1); /* mm's table pages, not a tree's ceiling */
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

/* A command to a child through the root's view of its work page (as its
 * parent would post it): its result. */
static uint64_t kid_cmd(struct work *w, uint64_t c, uint64_t a0, uint64_t a1, uint64_t a2)
{
    uint64_t seq = MMC_W(w, MMC_SEQ) + 1u;
    MMC_W(w, MMC_A0) = a0;
    MMC_W(w, MMC_A1) = a1;
    MMC_W(w, MMC_A2) = a2;
    MMC_W(w, MMC_CMD) = c;
    MMC_W(w, MMC_SEQ) = seq;
    for (unsigned i = 0; i < WAIT_ROUNDS && MMC_W(w, MMC_ACK) != seq; i++) {
        sleep_ns(SHORT_NS);
    }
    check(MMC_W(w, MMC_ACK) == seq, "a child didn't answer the root");
    return MMC_W(w, MMC_RES);
}

/* Orphans (0023's amendment of 2026-10-29): a fork cancelled, and a
 * pending child destroyed by its parent, whose index another client then
 * takes; then the parent exits with one child confirmed and one still
 * pending, and is destroyed. mm must destroy the pending child with it,
 * and nothing else: the other client stays, and the confirmed child is
 * still served, its pages now its alone. */
static void test_orphan(void)
{
    struct worker k = client(MMC_ORPHAN);
    at_mark(&k, 1, "the orphan client failed");
    expect_mm(1, POOL_PAGES - 8u, "a cancelled fork left a task in mm's records");
    expect_spaces(1, "a cancelled fork left a space");
    struct worker x = client(MMC_BASIC); /* in the destroyed child's index */
    wait_for(&x, 2);
    check(x.w->count == 2 && x.w->arg == 0, "the basic client failed");
    go_on(&k, 1);
    at_mark(&k, 3, "the orphan client failed");
    expect_mm(4, ANY_FREE, "mm's records after two forks are wrong");
    expect_spaces(4, "a clone's space isn't there");
    unsigned b = NRECS;
    for (unsigned i = 0; i < NRECS; i++) {
        if (recs[i].live && recs[i].kid) {
            check(b == NRECS, "the root started more than one child");
            b = i;
        }
    }
    check(b != NRECS, "the root didn't start the confirmed child");
    go_on(&k, 3);
    serve(&k, 99);
    check(k.w->count == 2 && k.w->arg == 0, "the orphan client failed at the end");

    must(j_task_destroy(k.task), "task_destroy (client)");
    sweep();
    expect_mm(2, ANY_FREE, "the child pending when its parent went wasn't destroyed, or more went");
    expect_spaces(2, "a child pending when its parent went is left, or more went");
    must(j_task_destroy(x.task), "task_destroy (the basic client)");
    sweep();
    expect_mm(1, POOL_PAGES - 8u, "mm's records with the confirmed child alone are wrong");
    struct work *bw = recs[b].w;
    check(kid_cmd(bw, MMC_C_CHECK, 0, 8, 1) == 0, "the confirmed child lost its pages");
    (void)kid_cmd(bw, MMC_C_FILL, 0, 8, 5);
    check(kid_cmd(bw, MMC_C_CHECK, 0, 8, 5) == 0, "the confirmed child's writes are lost");
    /* Its parent's and the other child's references went: no copies. */
    expect_mm(1, POOL_PAGES - 8u, "the parent's references outlived it");
    (void)kid_cmd(bw, MMC_C_EXIT, 0, 0, 0);
    finish_all();
    j_puts("mm: orphans ok\n");
}

/* Quota: a child made by mm spends its quota (MM_CHILD_QUOTA) and then
 * forks; the fork's thread is refused, the clone cancelled, and mm must
 * have destroyed that child. */
static void test_quota(void)
{
    struct worker k = client(MMC_QUOTA);
    at_mark(&k, 1, "the quota client failed");
    uint64_t n = MMC_W(k.w, MMC_RES);
    if (n != MM_CHILD_QUOTA) {
        j_puts("mm: the child's objects before its quota ran out ");
        j_putu(n);
        j_puts("\n");
        check(0, "a child's quota isn't MM_CHILD_QUOTA, or its cancelled fork failed");
    }
    expect_mm(2, POOL_PAGES - 4u, "a fork cancelled for want of quota left a task");
    expect_spaces(2, "a fork cancelled for want of quota left a space");
    go_on(&k, 1);
    serve(&k, 99);
    check(k.w->count == 2 && k.w->arg == 0, "the quota client failed at the end");
    finish(&k);
    j_puts("mm: quota ok\n");
}

/* Phase 5 step 3: a region capability (0030) asked for, grown and mapped
 * by the client itself; copied by a fork; freed. mm counts its pages. */
static void test_region(void)
{
    struct worker k = client(MMC_REGION);
    at_mark(&k, 1, "the region client failed");
    expect_mm(1, POOL_PAGES - 8u, "mm's records after REGION_GROW are wrong");
    go_on(&k, 1);
    at_mark(&k, 3, "the region client failed after its fork");
    expect_mm(2, POOL_PAGES - 16u, "a fork's child doesn't have its own region pages");
    expect_spaces(2, "a clone's space isn't there");
    go_on(&k, 3);
    at_mark(&k, 5, "the region client failed after REGION_FREE");
    expect_mm(1, POOL_PAGES, "a freed region's pages, or its child's, didn't come back");
    go_on(&k, 5);
    serve(&k, 99);
    check(k.w->count == 2 && k.w->arg == 0, "the region client failed at the end");
    finish(&k); /* its last region's page comes back with it */
    j_puts("mm: region ok\n");
}

/* A hog's pages when mm stops it: it touches one page after another, each
 * index in aux first, until mm can't serve a fault and passes it on as
 * SIGBUS (BUS_ADRERR, the owner's choice of 2026-11-21). Its exception
 * handler is the endpoint park, served here: the fault is checked (at the
 * page after the last it touched) and answered with an end, which ends the
 * hog's thread alone (any verdict but RESUME), so its task keeps its pages
 * (with no handler the whole task would end and give them back). */
static uint64_t hog_parked(struct worker *k, uint64_t park, uint64_t want)
{
    for (unsigned i = 0; i < WAIT_ROUNDS && k->w->aux < want; i++) {
        sleep_ns(SHORT_NS);
    }
    /* The fault brings the hog's thread capability: a slot of its own for
     * it, then the root's receive slots back where a child's capabilities
     * come (mmhost.h's start_kid), as test_passed does. */
    uint64_t th = slot_new();
    must(j_recv_slots(th, J_SLOT_NONE, 1), "recv_slots");
    struct j_msg m = {0};
    check(j_recv(park, &m, 2000u * J_MS) == J_OK, "a hog's fault wasn't passed on at its tree's limits");
    check((m.r[0] & J_FAULT_PASSED) != 0 && J_FAULT_CODE(m.r[0]) == J_BUS_ADRERR &&
              J_PASS_SIGNAL(J_FAULT_CODE(m.r[0])) == J_SIGBUS &&
              m.r[1] / J_PAGE == MMC_VA / J_PAGE + k->w->aux,
          "a hog's passed fault isn't SIGBUS/BUS_ADRERR at its next page");
    struct j_msg end = {.r = {1u}};
    must(j_reply(&end), "reply (a hog's fault: end its thread)");
    (void)j_cap_delete(th); /* E_EMPTY if none came */
    must(j_recv_slots(rx0, rx1, 2), "recv_slots");
    check(j_thread_suspend(k->thread) == J_E_GONE, "a hog's thread lived on after its handler ended it");
    return k->w->aux;
}

/* Phase 5 step 4 (0028), the phase's exit line: a fork bomb and two memory
 * hogs, each its own tree with the default limits, beside a fourth tree
 * that keeps faulting and forking, and is never refused. The bomb stops at
 * its task ceiling (its children pending, counted from CLONE); the first
 * hog at its page ceiling (half the pool); the second where the pool, less
 * the other trees' reservations, runs out. */
static void test_trees(void)
{
    struct worker s = client(MMC_STEADY);
    uint64_t sb = last_badge;
    at_mark(&s, 1, "the steady client failed");
    struct worker b = client(MMC_BOMB);
    at_mark(&b, 1, "the fork bomb failed");
    check(MMC_W(b.w, MMC_RES) == MM_MAX_TASKS / 2u - 1u, "the fork bomb wasn't stopped at its tree's task ceiling");
    /* Each hog's fault reaches an exception handler of the root's, which
     * ends its thread alone, so the hogs keep their pages: the second meets
     * the pool's end, and then every free page is reserved. */
    /* (pass_ep, the earlier tests' handler endpoint: a new object here
     * would leave the root's quota short of the steady client's forks.) */
    uint64_t park = pass_ep;
    next_handler_ep = park;
    next_handler_badge = 0x4061u;
    struct worker h1 = client(MMC_HOG);
    uint64_t p1 = hog_parked(&h1, park, POOL_PAGES / 2u);
    next_handler_ep = park;
    next_handler_badge = 0x4062u;
    struct worker h2 = client(MMC_HOG);
    uint64_t p2 = hog_parked(&h2, park, POOL_PAGES / 2u - 2u * MM_TREE_RESERVE);
    j_puts("mm: trees: bomb ");
    j_putu(MMC_W(b.w, MMC_RES));
    j_puts(" children, hogs ");
    j_putu(p1);
    j_puts(" and ");
    j_putu(p2);
    j_puts(" pages\n");
    check(p1 == POOL_PAGES / 2u && p2 == POOL_PAGES / 2u - 2u * MM_TREE_RESERVE,
          "a hog wasn't stopped where its tree's limits say");
    /* Every free page is reserved now: a fifth tree isn't admitted, and the
     * steady tree's reservation can't grow (MM_SET_LIMIT, the root's). */
    uint64_t x = slot_new();
    (void)ld_load(1, x);
    uint64_t at = MM_S_ROOT + nclients++;
    must(j_cap_give(mm_task, x, at, J_R_ALL), "cap_give (a fifth tree, to mm)");
    check(ask(MM_REQ_ADOPT, at).r[0] == MM_E_NOMEM, "a tree was admitted whose reservation doesn't fit");
    must(j_task_destroy(x), "task_destroy (a fifth tree)");
    struct j_msg l = {.r = {MM_REQ_SET_LIMIT, sb, (MM_TREE_RESERVE + 1u) | (uint64_t)POOL_PAGES << 32, MM_MAX_TASKS}};
    must(j_call(mm_root, &l, J_FOREVER), "call (mm)");
    check(l.r[0] == MM_E_NOMEM, "a reservation was raised past what is free");
    l = (struct j_msg){.r = {MM_REQ_SET_LIMIT, sb, MM_TREE_RESERVE | (uint64_t)(POOL_PAGES / 2u) << 32, MM_MAX_TASKS / 2u}};
    must(j_call(mm_root, &l, J_FOREVER), "call (mm)");
    check(l.r[0] == MM_OK, "SET_LIMIT refused limits that fit");
    go_on(&s, 1);
    serve(&s, 99);
    if (s.w->count != 2 || s.w->arg != 0) {
        j_puts("mm: trees: the steady client stopped at count ");
        j_putu(s.w->count);
        j_puts(", step ");
        j_putu(s.w->arg);
        j_puts("\n");
    }
    check(s.w->count == 2 && s.w->arg == 0, "the steady client was refused beside the bomb and the hogs");
    must(j_task_destroy(b.task), "task_destroy (bomb)");
    must(j_task_destroy(h1.task), "task_destroy (hog)");
    must(j_task_destroy(h2.task), "task_destroy (hog)");
    finish(&s); /* and the bomb's pending children with it, the pages back */
    j_puts("mm: trees ok\n");
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
    test_ended(MMC_GUARD, "guard page");
    pass_ep = slot_new();
    must(j_obj_create(J_ENDPOINT, pass_ep), "obj_create (exception handler)");
    test_passed();
    test_hung();
    test_fork();
    test_share();
    test_clone_full();
    test_orphan();
    test_quota();
    test_region();
    test_trees();
    /* Again, so that badges and table runs are used a second time. */
    test_basic();
    j_puts("mm: all ok\n");
    j_exit(0);
}
