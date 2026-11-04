/* SPDX-License-Identifier: GPL-3.0-only */
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
 * pages once more and exits, everything is destroyed, mm must have every
 * page back, no task must be left that mm made and nobody started (a child
 * still pending when its parent was killed mid-fork goes with its parent:
 * 0023's amendment of 2026-10-29), and the kernel's counters must be what they were after
 * the first round.
 *
 * Phase 5 step 3 (0030): after each step's operation, a second seeded
 * stream (so the first's choices are as they were) may act on a task's
 * region: make and grow it (mm), map the new pages (the task itself),
 * unmap a page and map it back (it keeps what it held), free it, or kill
 * the task while mm serves its region request. Region pages are written
 * and verified as the others; a fork's child gets a copy of its own. A
 * grow is asked only while mm has GROW_MARGIN pages free, for the copies
 * on write. Before the rounds, one task forks until mm, its records
 * full, refuses (so that mm has a table run for every index, as the root's
 * pages are warmed).
 *
 * Killing a task while mm serves it on that task's lent time leaves mm
 * with no time to finish (docs/abi.md: killing a thread takes back what it
 * lent); mm finishes on its fallback context (0025). After every kill the
 * root asks mm for its records with a timeout. With COW_RESCUE, if mm
 * doesn't answer, it was stranded, and the root lends it a context of its
 * own until it is back in its Recv. Without (the default), the root keeps
 * asking for STUCK_NS: a late answer is a slow mm (on a busy host, its
 * fallback's budget goes slowly), counted with the longest wait; no answer
 * at all is a stranded mm, and fails the test. The counts are printed.
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
#define COW_RESCUE 0 /* mm has a fallback (0025): a stranded mm is a failure */
#endif
#define MAX_LIVE     (MM_MAX_TASKS - 3u) /* room over, for the forks in flight */
#define MAX_KILL_FORKS 2u                /* kills mid-fork a round, at most */
#define MAX_MADE     24u                 /* tasks made in a round, at most */
#define WARM_PAGES   (MAX_MADE * 16u)    /* more than they take (it checks) */
#define WARM_RUNS    6u                  /* the loader's table runs: a round's first tasks */
#define BIG_WRITE    64u
#define POLL_NS      J_MS
#define BOUND_POLLS  20000u              /* 20 s: past it, a task has hung */
#define RESCUE_NS    (300u * J_MS)
#define STUCK_NS     (10000u * J_MS) /* no answer for this long: mm is stuck, not slow */
#define GROW_MARGIN  128u                /* mm's free pages a region grow leaves, at least */

static struct model {
    int live;
    unsigned depth;
    uint64_t seq;
    uint64_t slot[COW_PAGES + COW_RMAX][COW_SLOTS]; /* the region's pages after the others */
    int has;         /* it holds a region */
    unsigned rn;     /* its pages mapped, from its first */
    unsigned next;   /* its extents */
    unsigned ext_n[COW_REXTS];
} md[NRECS];

static uint64_t rs;  /* the seeded choices */
static uint64_t rr;  /* the region step's */
static uint64_t gen; /* the last generation handed out */
static unsigned round_no, max_depth, made, kill_forks;
static struct {
    uint64_t writes, forks, refused, exits, kills_fault, kills_fork, stranded, slow, slow_ms, quota, verifies;
    uint64_t rgrows, rrefused, rremaps, rfrees, rforks, kills_region;
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
    MMC_W(w, COW_RN) = md[i].rn;
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
    st.rforks += md[p].has != 0;
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

/* mm sound, and serving exactly the live tasks: a child pending when its
 * parent was killed is gone by now (the STATUS collects the parent). */
static void check_mm(void)
{
    struct j_msg r = ask(MM_REQ_STATUS, 0);
    unsigned n = live_count();
    if (r.r[0] != MM_OK || r.r[3] != 0 || r.r[1] != n) {
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
        for (unsigned p = 0; p < COW_PAGES + md[i].rn; p++) {
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
    uint64_t np = COW_PAGES + md[i].rn;
    for (uint64_t k = 0; k < n; k++) {
        md[i].slot[cow_next(&s0) % np][0] = cow_tag(i, g, 0);
        md[i].slot[cow_next(&s1) % np][1] = cow_tag(i, g, 1);
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

/* After a kill: mm answers at once, late (slow), or never (stranded
 * mid-request on the victim's time; with COW_RESCUE, lent a context until
 * it is back in its Recv). */
static void after_kill(void)
{
    struct j_msg m = {.r = {MM_REQ_STATUS, 0, 0, 0}};
    uint64_t e = j_call(mm_root, &m, RESCUE_NS);
    if (e == J_OK) {
        return;
    }
    check(e == J_E_TIMEOUT, "mm's STATUS failed after a kill");
#if COW_RESCUE
    st.stranded++;
    must(j_sc_bind(rescue_sc, mm_thread), "sc_bind (mm, rescued)");
    (void)ask(MM_REQ_STATUS, 0);
    must(j_sc_unbind(rescue_sc), "sc_unbind (mm, rescued)");
#else
    for (uint64_t waited = RESCUE_NS; waited < STUCK_NS; waited += RESCUE_NS) {
        m = (struct j_msg){.r = {MM_REQ_STATUS, 0, 0, 0}};
        e = j_call(mm_root, &m, RESCUE_NS);
        if (e == J_OK) {
            st.slow++;
            uint64_t ms = (waited + RESCUE_NS) / J_MS;
            st.slow_ms = ms > st.slow_ms ? ms : st.slow_ms;
            return;
        }
        check(e == J_E_TIMEOUT, "mm's STATUS failed after a kill");
    }
    st.stranded++;
    check(0, "mm stranded: no answer for 10 s after a client was killed while mm ran on its time");
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
    } else if (r == COW_E_QUOTA) {
        st.quota++; /* its clone cancelled: the next check_mm sees mm forgot it */
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
        kill_forks == MAX_KILL_FORKS || made >= MAX_MADE) {
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
    kill_forks++; /* its child, if mm made one, goes with it */
    made++;
    kill_task(v);
    st.kills_fork++;
}

/* ---- the region step (0030) ---- */

static uint64_t rrnd(uint64_t n)
{
    return cow_next(&rr) % n;
}

/* A random live task, from the region step's stream; with `has`, one that
 * holds a region. NRECS if none. */
static unsigned pick_r(int has)
{
    unsigned c[NRECS];
    unsigned n = 0;
    for (unsigned i = 0; i < NRECS; i++) {
        if (md[i].live && (!has || md[i].has)) {
            c[n++] = i;
        }
    }
    return n == 0 ? NRECS : c[rrnd(n)];
}

/* Whether task i's region may grow by n pages: room in it, and in mm. */
static int may_grow(unsigned i, uint64_t n)
{
    if (md[i].next == COW_REXTS || md[i].rn + n > COW_RMAX) {
        return 0;
    }
    return ask(MM_REQ_STATUS, 0).r[2] >= GROW_MARGIN + n;
}

/* Post a grow of task i's region by n pages (made first if it has none). */
static void post_grow(unsigned i, uint64_t n)
{
    post(i, COW_C_RGROW, n, !md[i].has, md[i].next);
}

/* Its region made and grown, or not, as the task answered. */
static void grown(unsigned i, uint64_t n)
{
    struct model *d = &md[i];
    uint64_t r = MMC_W(recs[i].w, MMC_RES);
    if (r == 0) {
        for (unsigned k = 0; k < n; k++) {
            for (unsigned s = 0; s < COW_SLOTS; s++) {
                d->slot[COW_PAGES + d->rn + k][s] = 0; /* mm zeroes them */
            }
        }
        d->has = 1;
        d->ext_n[d->next++] = (unsigned)n;
        d->rn += (unsigned)n;
        st.rgrows++;
    } else if (r == COW_E_MADE || r == COW_E_REFUSED) {
        d->has |= r == COW_E_MADE;
        st.rrefused++;
    } else {
        j_puts("cow: region grow: ");
        j_putu(r);
        j_puts("\n");
        fail_at(i, "a region grow failed");
    }
}

/* A region page unmapped and mapped again by the task: what it held stays. */
static void op_remap(unsigned i)
{
    uint64_t page = rrnd(md[i].rn);
    unsigned x = 0;
    uint64_t off = page;
    while (off >= md[i].ext_n[x]) {
        off -= md[i].ext_n[x++];
    }
    post(i, COW_C_RREMAP, page, x, off);
    wait_all();
    if (MMC_W(recs[i].w, MMC_RES) != 0) {
        fail_at(i, "a region page's unmap and map again failed");
    }
    st.rremaps++;
}

static void op_region(void)
{
    unsigned i = pick_r(0);
    if (i == NRECS) {
        return;
    }
    uint64_t n = 1u + rrnd(3);
    if (md[i].rn != 0 && (rrnd(3) == 0 || md[i].next == COW_REXTS || md[i].rn + n > COW_RMAX)) {
        op_remap(i);
        return;
    }
    if (!may_grow(i, n)) {
        return;
    }
    post_grow(i, n);
    wait_all();
    grown(i, n);
}

static void op_rfree(void)
{
    unsigned i = pick_r(1);
    if (i == NRECS) {
        return;
    }
    post(i, COW_C_RFREE, 0, 0, 0);
    wait_all();
    if (MMC_W(recs[i].w, MMC_RES) != 0) {
        fail_at(i, "a region free failed");
    }
    md[i].has = 0;
    md[i].rn = 0;
    md[i].next = 0;
    st.rfrees++;
}

/* Killed while mm serves its region request (a grow, or a free if it can't
 * grow), as the seed's delay falls: before, during or after it. */
static void op_kill_region(void)
{
    unsigned v = pick_r(0);
    if (v == NRECS || live_count() < 2u) {
        return;
    }
    if (may_grow(v, 1)) {
        post_grow(v, 1);
    } else if (md[v].has) {
        post(v, COW_C_RFREE, 0, 0, 0);
    } else {
        return;
    }
    uint64_t until = work_tsc() + rrnd(200000);
    while (work_tsc() < until && !acked(v)) {
        (void)j_yield();
    }
    kill_task(v);
    st.kills_region++;
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
    tree_unlimited(m.r[1]); /* the round's one tree: mm full is what it tests */
    give_root_cap(i);
    ld_stack(r->task, COW_HELPER_TOP);
    must(j_thread_spawn(r->task, r->thread, entry, CHILD_LO), "thread_spawn");
    must(j_thread_spawn(r->task, r->thread2, entry, COW_HELPER_TOP), "thread_spawn (helper)");
    must(j_sc_create(r->sc, HOST_PRIO, 10 * J_MS, 10 * J_MS), "sc_create");
    must(j_sc_bind(r->sc, r->thread), "sc_bind");
    must(j_sc_create(r->sc2, HOST_PRIO, 10 * J_MS, 10 * J_MS), "sc_create (helper)");
    must(j_sc_bind(r->sc2, r->thread2), "sc_bind (helper)");
    for (unsigned p = 0; p < COW_PAGES + COW_RMAX; p++) {
        for (unsigned s = 0; s < COW_SLOTS; s++) {
            md[i].slot[p][s] = 0;
        }
    }
    md[i].has = 0;
    md[i].rn = 0;
    md[i].next = 0;
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
 * destroyed, and the root must descend from no task but mm. */
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
        check(is_base(id), "an orphan is left: a task mm made that nobody started");
        id++;
    }
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
        rr = cow_mix(rs ^ 0x7265676e00000000ull);
        max_depth = 4u + round_no % 3u;
        kill_forks = 0;
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
            uint64_t rop = rrnd(100);
            if (rop < 20) {
                op_region();
            } else if (rop < 23) {
                op_rfree();
            } else if (rop < 26) {
                op_kill_region();
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
        put_stat(", slow ", st.slow);
        put_stat(" (at most ms ", st.slow_ms);
        put_stat("), refused ", st.refused);
        put_stat(", for quota ", st.quota);
        put_stat("; regions: grows ", st.rgrows);
        put_stat(" (refused ", st.rrefused);
        put_stat("), remaps ", st.rremaps);
        put_stat(", frees ", st.rfrees);
        put_stat(", forked ", st.rforks);
        put_stat(", kills mid-request ", st.kills_region);
        j_puts("\n");
    }
    check(st.rgrows != 0 && st.rforks != 0 && st.rremaps != 0,
          "the region writer never grew, forked or remapped a region");
    put_stat("cow: all ok: verified ", st.verifies);
    put_stat(" times, forks ", st.forks);
    put_stat(", deepest ", st.deepest);
    put_stat(", mm stranded ", st.stranded);
    put_stat(", slow ", st.slow);
    put_stat(" (at most ms ", st.slow_ms);
    j_puts(")\n");
    j_exit(0);
}
