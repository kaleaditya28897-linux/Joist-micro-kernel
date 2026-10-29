/* cowstress - Phase 4's exit test (docs/phase4-plan.md, *Testing*: "In
 * QEMU and on the P53"). Module 0 is mm, module 1 cowworker.
 *
 * Rounds of fork trees through mm's CLONE, 4 to 6 deep. A round starts
 * with one task the root loads and hands to mm; then, step by step, chosen
 * from the seed: some tasks write (both threads of each, the same pages,
 * their own slots: COW faults on the same pages at once on several CPUs),
 * a task forks (its child gets its pages copy-on-write), a task exits, or a
 * task is killed while it writes (mid-fault) or from just before its CLONE
 * (mid-fork). The root keeps what every slot of every task must hold (a
 * child starts with its parent's), and after every step every task reads
 * all its pages back and the root compares; mm's records must be sound and
 * serve the tasks there are. At the end of a round every task reads its
 * pages once more and exits, everything is destroyed, a child left with
 * nobody to start it (its parent killed mid-fork) is reaped, mm must have
 * every page back, and the kernel's counters must be what they were after
 * the first round. Before the rounds, one task forks until mm, its records
 * full, refuses (so that mm has a table run for every index, as the root's
 * pages are warmed).
 *
 * Killing a task while mm serves it on that task's lent time leaves mm
 * with no time to finish (docs/abi.md: killing a thread takes back what it
 * lent). After every kill the root asks mm for its records with a timeout;
 * if mm doesn't answer, it was stranded, and the root lends it a context of
 * its own until it is back in its Recv (COW_RESCUE), or fails the test
 * (COW_RESCUE=0). The count is printed.
 *
 * COW_ROUNDS rounds of COW_STEPS steps, from COW_SEED. Workers run at the
 * root's priority, so the root's waits (yields) let them run on one CPU. */
#define TEST_NAME "cow"
#include "../lib/testroot.h"
#include "../lib/cow.h"
#define HOST_POOL_PAGES 512u
#define HOST_TABLE_RUNS MM_MAX_TASKS
#define HOST_HELPER_TOP COW_HELPER_TOP
#define HOST_PRIO       128u
#include "../lib/mmhost.h"

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus);

#ifndef COW_ROUNDS
#define COW_ROUNDS 20u
#endif
#ifndef COW_STEPS
#define COW_STEPS 60u
#endif
#ifndef COW_SEED
#define COW_SEED 1u
#endif
#ifndef COW_RESCUE
#define COW_RESCUE 1
#endif
#define MAX_LIVE     (MM_MAX_TASKS - 3u) /* room for the orphans of a round */
#define MAX_ORPHANS  2u                  /* kills mid-fork a round, at most */
#define MAX_MADE     24u                 /* tasks made in a round, at most */
#define WARM_PAGES   (MAX_MADE * 16u)    /* more than they take (it checks) */
#define WARM_RUNS    6u                  /* the loader's table runs: a round's first tasks */
#define BIG_WRITE    64u
#define POLL_NS      J_MS
#define BOUND_POLLS  20000u              /* 20 s: past it, a task has hung */
#define RESCUE_NS    (300u * J_MS)

static struct model {
    int live;
    unsigned depth;
    uint64_t seq;
    uint64_t slot[COW_PAGES][COW_SLOTS];
} md[NRECS];

static uint64_t rs;  /* the seeded choices */
static uint64_t gen; /* the last generation handed out */
static unsigned round_no, max_depth, made, orphans_max;
static struct {
    uint64_t writes, forks, refused, exits, kills_fault, kills_fork, stranded, orphans, verifies;
    unsigned deepest;
} st;
static uint64_t rescue_sc;
static uint64_t base_ids[8]; /* the root's descendants before the rounds: mm */
static unsigned nbase;

static uint64_t rnd(uint64_t n)
{
    return cow_next(&rs) % n;
}

static void fail_at(unsigned i, const char *what)
{
    j_puts("cow: round ");
    j_putu(round_no);
    j_puts(", task ");
    j_putu(i);
    j_puts(": ");
    check(0, what);
}

static void post(unsigned i, uint64_t c, uint64_t a0, uint64_t a1, uint64_t a2)
{
    struct work *w = recs[i].w;
    MMC_W(w, MMC_A0) = a0;
    MMC_W(w, MMC_A1) = a1;
    MMC_W(w, MMC_A2) = a2;
    MMC_W(w, MMC_CMD) = c;
    MMC_W(w, MMC_SEQ) = ++md[i].seq;
}

static int acked(unsigned i)
{
    return MMC_W(recs[i].w, MMC_ACK) == md[i].seq;
}

/* A child its parent started: it begins with its parent's pages. */
static void kid_started(unsigned k)
{
    unsigned p = recs[k].parent;
    recs[k].w->arg = k;
    md[k] = md[p];
    md[k].live = 1;
    md[k].seq = 0;
    md[k].depth = md[p].depth + 1u;
    st.deepest = md[k].depth > st.deepest ? md[k].depth : st.deepest;
    st.forks++;
    made++;
}

/* Wait for every live task's answer, starting the children asked for. */
static void wait_all(void)
{
    for (unsigned n = 0;; n++) {
        unsigned k;
        if (host_serve(1, &k) && k != NRECS) {
            kid_started(k);
        }
        int all = 1;
        for (unsigned i = 0; i < NRECS; i++) {
            all &= !md[i].live || acked(i);
        }
        if (all) {
            return;
        }
        if (n == BOUND_POLLS * 100u) {
            for (unsigned i = 0; i < NRECS; i++) {
                if (md[i].live && !acked(i)) {
                    fail_at(i, "didn't answer: hung");
                }
            }
        }
        (void)j_yield();
    }
}

static unsigned live_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < NRECS; i++) {
        n += md[i].live != 0;
    }
    return n;
}

/* mm sound, serving the live tasks and at most the orphans there may be. */
static void check_mm(void)
{
    struct j_msg r = ask(MM_REQ_STATUS, 0);
    unsigned n = live_count();
    if (r.r[0] != MM_OK || r.r[3] != 0 || r.r[1] < n || r.r[1] > n + orphans_max) {
        j_puts("cow: mm serves ");
        j_putu(r.r[1]);
        j_puts(" tasks for ");
        j_putu(n);
        j_puts(", its check finds ");
        j_putu(r.r[3]);
        j_puts("\n");
        fail_at(0, "mm's records are wrong");
    }
}

/* Every live task reads all its pages; each must be what the root wrote. */
static void verify_all(void)
{
    for (unsigned i = 0; i < NRECS; i++) {
        if (md[i].live) {
            post(i, COW_C_VERIFY, 0, 0, 0);
        }
    }
    wait_all();
    for (unsigned i = 0; i < NRECS; i++) {
        if (!md[i].live) {
            continue;
        }
        for (unsigned p = 0; p < COW_PAGES; p++) {
            for (unsigned s = 0; s < COW_SLOTS; s++) {
                uint64_t got = COW_OBS(recs[i].w, p, s);
                if (got != md[i].slot[p][s]) {
                    j_puts("cow: page ");
                    j_putu(p);
                    j_puts(" slot ");
                    j_putu(s);
                    j_puts(": read ");
                    j_putu(got);
                    j_puts(", wrote ");
                    j_putu(md[i].slot[p][s]);
                    j_puts("\n");
                    fail_at(i, "a page doesn't hold what was written");
                }
            }
        }
    }
    st.verifies++;
    check_mm();
}

/* What a write command does to the root's record of task i's pages. */
static void model_write(unsigned i, uint64_t n, uint64_t seed, uint64_t g)
{
    uint64_t s0 = seed;
    uint64_t s1 = COW_SEED2(seed);
    for (uint64_t k = 0; k < n; k++) {
        md[i].slot[cow_next(&s0) % COW_PAGES][0] = cow_tag(i, g, 0);
        md[i].slot[cow_next(&s1) % COW_PAGES][1] = cow_tag(i, g, 1);
    }
}

static void finish_destruction(void)
{
    int more = 1;
    for (unsigned i = 0; more && i < 1000u; i++) {
        must(j_continue(0, &more), "continue");
    }
    check(!more, "a destruction didn't finish");
}

/* After a kill: mm answers, or was stranded mid-request on the victim's
 * time and is lent a context until it is back in its Recv. */
static void after_kill(void)
{
    struct j_msg m = {.r = {MM_REQ_STATUS, 0, 0, 0}};
    uint64_t e = j_call(mm_root, &m, RESCUE_NS);
    if (e == J_OK) {
        return;
    }
    check(e == J_E_TIMEOUT, "mm's STATUS failed after a kill");
    st.stranded++;
#if COW_RESCUE
    must(j_sc_bind(rescue_sc, mm_thread), "sc_bind (mm, rescued)");
    (void)ask(MM_REQ_STATUS, 0);
    must(j_sc_unbind(rescue_sc), "sc_unbind (mm, rescued)");
#else
    check(0, "mm stranded: a client was killed while mm ran on its time");
#endif
}

static void kill_task(unsigned v)
{
    must(j_task_destroy(recs[v].task), "task_destroy (a victim)");
    finish_destruction();
    md[v].live = 0;
    rec_free(v);
    after_kill();
}

/* A random live task, or NRECS; with `deep`, one that may fork. */
static unsigned pick(int deep)
{
    unsigned c[NRECS];
    unsigned n = 0;
    for (unsigned i = 0; i < NRECS; i++) {
        if (md[i].live && (!deep || md[i].depth < max_depth)) {
            c[n++] = i;
        }
    }
    return n == 0 ? NRECS : c[rnd(n)];
}

static void op_write(void)
{
    int any = 0;
    for (unsigned i = 0; i < NRECS; i++) {
        if (md[i].live && (rnd(2) == 0 || !any)) {
            uint64_t n = 1u + rnd(16);
            uint64_t seed = cow_next(&rs);
            post(i, COW_C_WRITE, n, seed, ++gen);
            model_write(i, n, seed, gen);
            st.writes += 2u * n;
            any = 1;
        }
    }
    wait_all();
}

static void op_fork(void)
{
    /* The deepest that may fork, three times in four: trees grow deep. */
    unsigned p = pick(1);
    if (p == NRECS || live_count() >= MAX_LIVE || made >= MAX_MADE) {
        return;
    }
    if (rnd(4) != 0) {
        for (unsigned i = 0; i < NRECS; i++) {
            if (md[i].live && md[i].depth < max_depth && md[i].depth > md[p].depth) {
                p = i;
            }
        }
    }
    post(p, COW_C_FORK, 0, 0, 0);
    wait_all();
    uint64_t r = MMC_W(recs[p].w, MMC_RES);
    if (r == COW_E_REFUSED) {
        st.refused++;
    } else if (r != 0) {
        j_puts("cow: fork: ");
        j_putu(r);
        j_puts("\n");
        fail_at(p, "a fork failed");
    }
}

static void op_exit(void)
{
    unsigned v = pick(0);
    if (v == NRECS || live_count() < 2u) {
        return;
    }
    post(v, COW_C_EXIT, 0, 0, 0);
    wait_all();
    for (unsigned n = 0; recs[v].w->count != 2; n++) {
        if (n == BOUND_POLLS * 100u) {
            fail_at(v, "didn't exit");
        }
        (void)j_yield();
    }
    must(j_task_destroy(recs[v].task), "task_destroy (exited)");
    finish_destruction();
    md[v].live = 0;
    rec_free(v);
    st.exits++;
}

/* Killed while it writes, at a write chosen from the seed. */
static void op_kill_fault(void)
{
    unsigned v = pick(0);
    if (v == NRECS || live_count() < 2u) {
        return;
    }
    uint64_t at = rnd(BIG_WRITE);
    MMC_W(recs[v].w, COW_PROG) = 0;
    post(v, COW_C_WRITE, BIG_WRITE, cow_next(&rs), ++gen);
    for (unsigned n = 0; MMC_W(recs[v].w, COW_PROG) < at && !acked(v); n++) {
        if (n == BOUND_POLLS * 100u) {
            fail_at(v, "didn't get to its kill point");
        }
        (void)j_yield();
    }
    kill_task(v);
    st.kills_fault++;
}

/* Killed from just before its CLONE: in mm's clone, between the clone and
 * the start of its child, or as it starts it, as the seed's delay falls. */
static void op_kill_fork(void)
{
    unsigned v = pick(1);
    if (v == NRECS || live_count() < 2u || live_count() >= MAX_LIVE ||
        orphans_max == MAX_ORPHANS || made >= MAX_MADE) {
        return;
    }
    MMC_W(recs[v].w, COW_INCLONE) = 0;
    post(v, COW_C_FORK, 0, 0, 0);
    for (unsigned n = 0; MMC_W(recs[v].w, COW_INCLONE) == 0 && !acked(v); n++) {
        if (n == BOUND_POLLS * 100u) {
            fail_at(v, "didn't get to its CLONE");
        }
        (void)j_yield();
    }
    uint64_t until = work_tsc() + rnd(200000);
    while (work_tsc() < until) {
        (void)j_yield();
    }
    orphans_max++; /* its child, if mm made one, has nobody to start it */
    made++;
    kill_task(v);
    st.kills_fork++;
}

/* The round's first task: loaded here, handed to mm, both threads run. */
static void first_task(void)
{
    unsigned i = rec_new();
    struct rec *r = &recs[i];
    r->kid = 1;
    r->parent = i;
    uint64_t entry = ld_load(1, r->task);
    struct work *w = rec_page(i);
    w->role = 0;
    w->arg = i;
    must(j_cap_give(mm_task, r->task, MM_S_ROOT, J_R_ALL), "cap_give (to mm)");
    struct j_msg m = ask(MM_REQ_ADOPT, MM_S_ROOT);
    check(m.r[0] == MM_OK, "mm's ADOPT failed");
    give_root_cap(i);
    ld_stack(r->task, COW_HELPER_TOP);
    must(j_thread_spawn(r->task, r->thread, entry, CHILD_LO), "thread_spawn");
    must(j_thread_spawn(r->task, r->thread2, entry, COW_HELPER_TOP), "thread_spawn (helper)");
    must(j_sc_create(r->sc, HOST_PRIO, 10 * J_MS, 10 * J_MS), "sc_create");
    must(j_sc_bind(r->sc, r->thread), "sc_bind");
    must(j_sc_create(r->sc2, HOST_PRIO, 10 * J_MS, 10 * J_MS), "sc_create (helper)");
    must(j_sc_bind(r->sc2, r->thread2), "sc_bind (helper)");
    for (unsigned p = 0; p < COW_PAGES; p++) {
        for (unsigned s = 0; s < COW_SLOTS; s++) {
            md[i].slot[p][s] = 0;
        }
    }
    md[i].live = 1;
    md[i].seq = 0;
    md[i].depth = 0;
    made++;
}

static int is_base(uint64_t id)
{
    for (unsigned i = 0; i < nbase; i++) {
        if (base_ids[i] == id) {
            return 1;
        }
    }
    return 0;
}

/* Every task gone: the live ones read their pages and exit, then all are
 * destroyed, and any other the root descends from but mm reaped. */
static void round_end(void)
{
    verify_all();
    for (unsigned i = 0; i < NRECS; i++) {
        if (md[i].live) {
            post(i, COW_C_EXIT, 0, 0, 0);
        }
    }
    wait_all();
    for (unsigned i = 0; i < NRECS; i++) {
        if (md[i].live) {
            md[i].live = 0;
            rec_free(i);
        }
    }
    finish_destruction();
    uint64_t id = 0;
    while (j_task_descendant(id, &id) == J_OK) {
        if (!is_base(id)) {
            must(j_sys4(J_SYS_TASK_REAP, id, 0, 0, 0), "task_reap (an orphan)");
            st.orphans++;
        }
        id++;
    }
    finish_destruction();
    expect_mm(0, HOST_POOL_PAGES, "mm didn't get every page back at the end of a round");
}

/* Before the rounds: one task forks until mm refuses, its records full, so
 * that mm has split a table run for every index (as the root's pages are
 * warmed). Every page is verified, then everything goes. */
static void warm_mm(void)
{
    made = 0;
    uint32_t mark = ld_mark();
    first_task();
    verify_all(); /* its pages there, so that the children share them */
    unsigned t0 = 0;
    while (!md[t0].live) {
        t0++;
    }
    for (unsigned n = 0;; n++) {
        check(n < MM_MAX_TASKS, "mm never refused a CLONE, its records full");
        post(t0, COW_C_FORK, 0, 0, 0);
        wait_all();
        uint64_t r = MMC_W(recs[t0].w, MMC_RES);
        if (r == COW_E_REFUSED) {
            break;
        }
        check(r == 0, "a fork failed while mm had room");
    }
    expect_mm(MM_MAX_TASKS, ANY_FREE, "mm refused a CLONE before its records were full");
    verify_all();
    round_end();
    ld_release(mark);
    st.forks = 0;
    j_puts("cow: mm full, refused ok\n");
}

static void counters_level(const struct j_counters *base, const char *when)
{
    struct j_counters c;
    must(j_debug_counters(&c), "debug_counters");
    if (c.objects != base->objects || c.threads != base->threads || c.spaces != base->spaces ||
        c.leaves != base->leaves || c.mappings != base->mappings || c.pending != base->pending ||
        c.table_pool != base->table_pool || c.frames_free != base->frames_free ||
        c.table_pages != base->table_pages) {
        j_puts("cow: counters (objects, threads, spaces, leaves, mappings, pending, table pool, "
               "free frames, table pages): ");
        const uint64_t a[] = {c.objects, c.threads, c.spaces, c.leaves, c.mappings, c.pending,
                              c.table_pool, c.frames_free, c.table_pages};
        const uint64_t b[] = {base->objects, base->threads, base->spaces, base->leaves,
                              base->mappings, base->pending, base->table_pool,
                              base->frames_free, base->table_pages};
        for (unsigned i = 0; i < 9; i++) {
            j_putu(a[i]);
            j_puts(i == 8 ? " against " : " ");
        }
        for (unsigned i = 0; i < 9; i++) {
            j_putu(b[i]);
            j_puts(i == 8 ? "\n" : " ");
        }
        check(0, when);
    }
}

static void put_stat(const char *name, uint64_t v)
{
    j_puts(name);
    j_putu(v);
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 2) {
        die("needs two modules: mm, cowworker");
    }
    test_init();
    (void)sleep_ns; /* (the waits here yield: the workers share the root's priority) */
    host_init();
    start_mm();
    rescue_sc = slot_new();
    must(j_sc_create(rescue_sc, 200, 10 * J_MS, 10 * J_MS), "sc_create (rescue)");
    uint64_t id = 0;
    while (j_task_descendant(id, &id) == J_OK && nbase < 8) {
        base_ids[nbase++] = id++;
    }
    /* The pages every round takes, split off the pool once. */
    uint32_t warm = ld_mark();
    for (unsigned i = 0; i < WARM_PAGES; i++) {
        (void)page_alloc();
    }
    ld_release(warm);
    ld_warm_runs(WARM_RUNS);
    j_puts("cow: cpus ");
    j_putu(ncpus);
    put_stat(", rounds ", COW_ROUNDS);
    put_stat(", steps ", COW_STEPS);
    put_stat(", seed ", COW_SEED);
    j_puts("\n");
    warm_mm();
    struct j_counters base;
    for (round_no = 0; round_no < COW_ROUNDS; round_no++) {
        rs = cow_mix((uint64_t)COW_SEED << 32 | round_no);
        max_depth = 4u + round_no % 3u;
        orphans_max = 0;
        made = 0;
        uint32_t mark = ld_mark();
        first_task();
        verify_all();
        for (unsigned s = 0; s < COW_STEPS; s++) {
            uint64_t op = rnd(100);
            if (op < 40) {
                op_write();
            } else if (op < 70) {
                op_fork();
            } else if (op < 80) {
                op_exit();
            } else if (op < 90) {
                op_kill_fault();
            } else {
                op_kill_fork();
            }
            if (live_count() == 0) {
                if (ld_runs_since(mark) == WARM_RUNS) {
                    break; /* the round's first tasks have all gone: it ends early */
                }
                first_task();
            }
            verify_all();
        }
        round_end();
        check(ld_pages_since(mark) <= WARM_PAGES, "a round took more pages than were warmed");
        ld_release(mark);
        if (round_no == 0) {
            must(j_debug_counters(&base), "debug_counters");
        } else {
            counters_level(&base, "the kernel's counters aren't level after a round");
        }
        j_puts("cow: round ");
        j_putu(round_no);
        put_stat(" ok: forks ", st.forks);
        put_stat(", deepest ", st.deepest);
        put_stat(", writes ", st.writes);
        put_stat(", exits ", st.exits);
        put_stat(", kills mid-fault ", st.kills_fault);
        put_stat(", mid-fork ", st.kills_fork);
        put_stat(", mm stranded ", st.stranded);
        put_stat(", orphans reaped ", st.orphans);
        put_stat(", refused ", st.refused);
        j_puts("\n");
    }
    put_stat("cow: all ok: verified ", st.verifies);
    put_stat(" times, forks ", st.forks);
    put_stat(", deepest ", st.deepest);
    put_stat(", mm stranded ", st.stranded);
    j_puts("\n");
    j_exit(0);
}
