/* growroot.c - the memory-growth test (Phase 3's exit: "no kernel memory
 * growth under adversarial tests"; docs/phase3-plan.md, *Testing*). Module
 * 0 is fuzzchild.
 *
 * Round after round, seeded, it makes and destroys tasks, threads,
 * endpoints, notifications, scheduling contexts, capabilities and mappings,
 * in loops and in cycles of tasks that hold each other; drives each kind of
 * fixed table to exhaustion; and lets a child make system calls at random.
 * After every round everything it made is gone, and the kernel's counters
 * (kernel/sys/abi.h, struct abi_counters: what is in use in each table, and
 * what is free in the kernel's own pools) must read exactly what they read
 * after the first. Something the kernel failed to give back would show as a
 * count that creeps.
 *
 * The root's own capabilities live in a fixed set of slots, reused every
 * round, and the pages it gives tasks are taken back (lib/loader.h,
 * ld_release), so its own use stays level too. */
#define TEST_NAME "grow"
#include "../lib/testroot.h"

#ifndef GROW_ROUNDS
#define GROW_ROUNDS 40u
#endif
#ifndef GROW_SEED
#define GROW_SEED 1u
#endif
#define NTASK 6u
/* Past any table's size (the kernel's objects: 2 048, kernel/Makefile KSIZES):
 * exhaustion is reached, not assumed. */
#define NOBJ 2200u
#define WORK_VA_PAGE (0x40000000ull / J_PAGE)

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus);

static uint64_t rng;
static uint64_t rnd(uint64_t n)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return (rng * 0x2545F4914F6CDD1DULL >> 11) % n;
}

static void put(const char *what, uint64_t v)
{
    j_puts(what);
    j_putu(v);
}

/* The root's working slots, made once. */
static uint64_t task[NTASK], thread[NTASK], sc[NTASK], ep, ntf, tmp;
static uint64_t obj[NOBJ];
static uint64_t cycles; /* cycles of tasks made (and so reaped) */

static void counters(struct j_counters *c)
{
    must(j_debug_counters(c), "debug_counters");
}

static void drain(void)
{
    int more = 1;
    while (more) {
        must(j_continue(0, &more), "continue");
    }
}

/* Delete whatever is in a slot of the root's (an empty slot is fine). */
static void clear(uint64_t slot)
{
    uint64_t e = j_cap_delete(slot);
    check(e == J_OK || e == J_E_EMPTY, "cap_delete of a working slot failed");
}

/* Make and destroy tasks in every way there is. */
static void churn(int all)
{
    uint32_t mark = ld_mark();
    uint64_t n = all ? NTASK : 1u + rnd(NTASK);
    must(j_obj_create(J_ENDPOINT, ep), "obj_create (endpoint)");
    must(j_obj_create(J_NOTIFICATION, ntf), "obj_create (notification)");
    for (uint64_t i = 0; i < n; i++) {
        uint64_t e = ld_load(0, task[i]);
        /* Capabilities into it: the endpoint both ways, a badged notification. */
        must(j_cap_give(task[i], ep, 0, J_R_ALL), "cap_give"); /* (before its thread runs) */
        must(j_cap_mint(ntf, tmp, J_R_SEND | J_R_GRANT, 1u + i), "cap_mint");
        must(j_cap_give(task[i], tmp, 1, J_R_SEND), "cap_give");
        clear(tmp);
        must(j_thread_spawn(task[i], thread[i], e, CHILD_LO), "thread_spawn");
        if (rnd(2) == 0) {
            /* Give it time of its own and let it make calls at random. */
            struct j_thread_regs regs;
            must(j_thread_read_regs(thread[i], &regs), "thread_read_regs");
            regs.r[J_RDI] = rnd(1u << 30);
            must(j_thread_write_regs(thread[i], &regs), "thread_write_regs");
            must(j_sc_create(sc[i], 50u + rnd(60), J_MS, 2 * J_MS), "sc_create");
            must(j_sc_bind(sc[i], thread[i]), "sc_bind");
        }
    }
    /* Cycles: tasks that hold each other's capabilities. */
    /* (A fuzzing child may have filled the slot, or ended its own task,
     * by now: then that cycle isn't made, which is counted.) */
    for (uint64_t i = 0; i + 1u < n; i += 2) {
        cycles += j_cap_give(task[i], task[i + 1u], 60, J_R_ALL) == J_OK &&
                  j_cap_give(task[i + 1u], task[i], 60, J_R_ALL) == J_OK;
    }
    sleep_ns(rnd(20) * J_MS); /* the fuzzing children run meanwhile */
    /* Gone, each its own way: destroyed; its last capability dropped (in a
     * cycle, it lives on held by its partner, until reaped); its thread
     * killed first. Then everything that remains below the root, reaped. */
    for (uint64_t i = 0; i < n; i++) {
        switch (rnd(3)) {
        case 0:
            (void)j_task_destroy(task[i]);
            break;
        case 1:
            (void)j_thread_kill(thread[i]);
            break;
        default:
            break;
        }
        clear(task[i]);
        clear(thread[i]);
        clear(sc[i]);
    }
    drain();
    for (int found = 1; found;) {
        struct j_regs r = {.rax = J_SYS_TASK_DESCENDANT, .rdi = 0};
        j_syscall(&r);
        found = r.rax == J_OK;
        if (found) {
            must(j_sys4(J_SYS_TASK_REAP, r.rdx, 0, 0, 0), "task_reap");
            drain();
        }
    }
    clear(ep);
    clear(ntf);
    drain();
    ld_release(mark);
}

/* Fill a table until the kernel says no, then empty it. */
static void exhaust(void)
{
    uint64_t n = 0;
    uint64_t e = J_OK;
    uint64_t type = rnd(2) ? J_ENDPOINT : J_NOTIFICATION;
    while (n < NOBJ && (e = j_obj_create(type, obj[n])) == J_OK) {
        n++;
    }
    check(e == J_E_NOMEM, "filling the object table didn't end in E_NOMEM");
    for (uint64_t i = 0; i < n; i++) {
        clear(obj[i]);
    }
    /* Tasks until the table of spaces (or the quota) is full. */
    n = 0;
    while (n < NOBJ && (e = j_task_create(2, obj[n])) == J_OK) {
        n++;
    }
    check(e == J_E_NOMEM, "filling the space table didn't end in E_NOMEM");
    for (uint64_t i = 0; i < n; i++) {
        must(j_task_destroy(obj[i]), "task_destroy");
        clear(obj[i]);
    }
    drain();
    /* Page tables until the pages lent to a task of its own run out (0021):
     * one page, mapped again and again, a last-level table (2 MiB) apart so
     * that every map needs a new table. */
    uint32_t mark = ld_mark();
    must(j_task_create(64, task[0]), "task_create");
    ld_tables(task[0]);
    uint64_t page = page_alloc();
    n = 0;
    while (n < NOBJ && (e = j_map(page, task[0], WORK_VA_PAGE + n * J_MAP_SPAN, 0)) == J_OK) {
        n++;
    }
    check(e == J_E_NOMEM, "filling the lent pages with tables didn't end in E_NOMEM");
    for (uint64_t i = 0; i < n; i += 2) {
        must(j_unmap(task[0], WORK_VA_PAGE + i * J_MAP_SPAN), "unmap"); /* half by unmapping */
    }
    must(j_task_destroy(task[0]), "task_destroy"); /* the rest with the task */
    clear(task[0]);
    drain();
    ld_release(mark);
}

static int same(const struct j_counters *a, const struct j_counters *b)
{
    return a->objects == b->objects && a->threads == b->threads && a->spaces == b->spaces &&
           a->leaves == b->leaves && a->mappings == b->mappings && a->pending == b->pending &&
           a->table_pool == b->table_pool && a->frames_free == b->frames_free &&
           a->table_pages == b->table_pages;
}

static void show(const char *what, const struct j_counters *c)
{
    j_puts(what);
    put(" objects ", c->objects);
    put(", threads ", c->threads);
    put(", spaces ", c->spaces);
    put(", leaves ", c->leaves);
    put(", mappings ", c->mappings);
    put(", pending ", c->pending);
    put(", page-table frames free ", c->table_pool);
    put(", lent table pages used ", c->table_pages);
    put(", frames free ", c->frames_free);
    put(", run-queue walks so far ", c->rq_walks); /* (grows: not compared) */
    j_puts("\n");
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus)
{
    (void)ncpus;
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 1) {
        die("needs one module: fuzzchild");
    }
    test_init();
    rng = GROW_SEED * 0x9E3779B97F4A7C15ULL + 7u;
    for (unsigned i = 0; i < NTASK; i++) {
        task[i] = slot_new();
        thread[i] = slot_new();
        sc[i] = slot_new();
    }
    ep = slot_new();
    ntf = slot_new();
    tmp = slot_new();
    for (unsigned i = 0; i < NOBJ; i++) {
        obj[i] = slot_new();
    }
    put("grow: seed ", GROW_SEED);
    put(", ", GROW_ROUNDS);
    j_puts(" rounds\n");

    /* A first round of each, at the most that any round takes (every task
     * there can be), so that whatever is made once and kept (the root's
     * table leaves, the pages split off the pool) is made; then the
     * counters. */
    churn(1);
    exhaust();
    struct j_counters base;
    struct j_counters now;
    counters(&base);
    show("grow: after the first round:", &base);
    for (unsigned r = 1; r <= GROW_ROUNDS; r++) {
        churn(0);
        if (r % 4u == 0) {
            exhaust();
        }
        counters(&now);
        if (!same(&base, &now)) {
            put("grow: round ", r);
            j_puts("\n");
            show("grow: now:", &now);
            check(0, "the kernel's counters moved");
        }
    }
    show("grow: after the last round:", &now);
    put("grow: ", GROW_ROUNDS + 1u);
    put(" rounds; cycles of tasks holding each other made and reaped: ", cycles);
    j_puts("\n");
    j_puts("grow: all ok\n");
    j_exit(0);
}
