/* SPDX-License-Identifier: GPL-3.0-only */
/* mmhost.h - what a root task that runs the memory manager needs (Phase 4
 * step 6; decision 0023): mm started passive, its records asked for, and
 * the root's side of how a client makes a child through mm (work.h,
 * MMC_RQ_START). Shared by mmroot and cowstress. Header only; the includer
 * includes testroot.h first, and may set HOST_POOL_PAGES, HOST_TABLE_RUNS,
 * HOST_PRIO, HOST_HELPER_TOP (a second thread for each child, its stack
 * below that) and HOST_MM_TABLES (more pages lent to mm for its own
 * tables) before including it.
 *
 * mm is module 0. The tasks that may ask the root for something hold a
 * capability to the root's endpoint in MMC_ROOT_SLOT, each with its own
 * badge (a record's index plus one): the clients the root loads, and the
 * children they make. */
#ifndef JOIST_USER_MMHOST_H
#define JOIST_USER_MMHOST_H

#include "mm_proto.h"
#include "mmlib.h"

#ifndef HOST_POOL_PAGES
#define HOST_POOL_PAGES 256u
#endif
#ifndef HOST_TABLE_RUNS
#define HOST_TABLE_RUNS MM_MAX_TASKS /* one per task record: mm refuses fewer (0028) */
#endif
#ifndef HOST_PRIO
#define HOST_PRIO 100u /* the children's threads' */
#endif
#define HOST_TABLE_PAGES (HOST_TABLE_RUNS * MM_TABLE_PAGES + 1u)
/* mm's quota: its leaf for the tasks it makes, its table runs, and each
 * task it makes (MM_CHILD_QUOTA and the task object), with room over. */
#define HOST_MM_DONATION (64u + MM_MAX_TASKS * (MM_CHILD_QUOTA + 1u))
#define HOST_VIEW_VA     0x38000000ull /* the root's view of each record's work page */
#define ANY_FREE         UINT64_MAX

static uint64_t mm_task, mm_thread, mm_ep, mm_root;

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

/* The tree of the task with this badge may have the whole pool and every
 * task record, with no reservation (0028): for a test whose subject is mm
 * being full, not a tree's limits. */
static inline void tree_unlimited(uint64_t badge)
{
    struct j_msg m = {.r = {MM_REQ_SET_LIMIT, badge, (uint64_t)HOST_POOL_PAGES << 32, MM_MAX_TASKS}};
    must(j_call(mm_root, &m, J_FOREVER), "call (mm)");
    check(m.r[0] == MM_OK, "mm's SET_LIMIT failed");
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
    uint64_t entry = ld_load_quota(0, mm_task, HOST_MM_DONATION);
#ifdef HOST_MM_TABLES
    /* More pages for its own tables: its pool's mapping spans more tables. */
    must(j_space_donate(mm_task, frame_take(HOST_MM_TABLES)), "space_donate (mm)");
#endif
    give(mm_ep, MM_S_EP, 0);
    give(mm_task, MM_S_SELF, 0);
    /* Frames mm alone holds: it maps one and splits the other. */
    give(frame_take(HOST_POOL_PAGES), MM_S_POOL, 1);
    give(frame_take(HOST_TABLE_PAGES), MM_S_TABLES, 1);

    mm_thread = slot_new();
    uint64_t sc = slot_new();
    must(j_thread_spawn(mm_task, mm_thread, entry, CHILD_LO), "thread_spawn (mm)");
    /* Passive with a fallback (0025): it runs on it now, to its first Recv,
     * and again only to finish a request whose caller left mid-way. */
    must(j_sc_create(sc, 200, 2 * J_MS, 10 * J_MS), "sc_create (mm's fallback)");
    must(j_sc_bind_fallback(sc, mm_thread), "sc_bind_fallback (mm)");
    struct j_msg r = ask(MM_REQ_INIT, 0);
    check(r.r[0] == MM_OK, "mm's INIT failed");
    r = ask(MM_REQ_INIT, 0);
    check(r.r[0] == MM_E_STATE, "mm took a second INIT");
    j_puts(TEST_NAME ": started, passive ok\n");
}

/* mm's records: tasks served, free pages (unless ANY_FREE), and its own
 * check. */
static __attribute__((unused)) void expect_mm(uint64_t tasks, uint64_t free, const char *what)
{
    struct j_msg r = ask(MM_REQ_STATUS, 0);
    check(r.r[0] == MM_OK && r.r[3] == 0, "mm's records are unsound");
    if (r.r[1] != tasks || (free != ANY_FREE && r.r[2] != free)) {
        j_puts(TEST_NAME ": tasks ");
        j_putu(r.r[1]);
        j_puts(", free pages ");
        j_putu(r.r[2]);
        j_puts("\n");
        check(0, what);
    }
}

#define NRECS 32u
static struct rec {
    uint64_t task, thread, sc;     /* the root's capabilities (thread and sc: a child's) */
    uint64_t thread2, sc2;         /* a child's second thread (HOST_HELPER_TOP) */
    uint64_t frame;                /* its work page; 0: none of the root's */
    struct work *w;                /* the root's view of it */
    unsigned parent;               /* the record of the task that made it */
    int live, kid;
} recs[NRECS];
static uint64_t root_ep, rx0, rx1, root_tmp;

/* After test_init: the root's endpoint, and where the capabilities of a
 * child arrive. */
static void host_init(void)
{
    root_ep = slot_new();
    must(j_obj_create(J_ENDPOINT, root_ep), "obj_create (the root's endpoint)");
    rx0 = slot_new();
    rx1 = slot_new();
    root_tmp = slot_new();
    must(j_recv_slots(rx0, rx1, 2), "recv_slots");
}

static unsigned rec_new(void)
{
    for (unsigned i = 0; i < NRECS; i++) {
        struct rec *r = &recs[i];
        if (!r->live) {
            if (r->task == 0) {
                r->task = slot_new();
                r->thread = slot_new();
                r->sc = slot_new();
                r->thread2 = slot_new();
                r->sc2 = slot_new();
            }
            r->live = 1;
            r->kid = 0;
            r->frame = 0;
            r->w = 0;
            return i;
        }
    }
    die("too many tasks for the root's records");
}

/* Give the task of record i its capability to the root. */
static void give_root_cap(unsigned i)
{
    must(j_cap_mint(root_ep, root_tmp, J_R_SEND | J_R_GRANT, i + 1u),
         "cap_mint (the root's endpoint)");
    must(j_cap_give(recs[i].task, root_tmp, MMC_ROOT_SLOT, J_R_SEND),
         "cap_give (the root's endpoint)");
    must(j_cap_delete(root_tmp), "cap_delete (the root's endpoint)");
}

/* A work page for record i, zeroed, in the root's view and in its task. */
static struct work *rec_page(unsigned i)
{
    struct rec *r = &recs[i];
    r->frame = page_alloc();
    uint64_t va = HOST_VIEW_VA + i * J_PAGE;
    must(j_map(r->frame, J_ROOT_SELF, va / J_PAGE, J_MAP_W), "map (a work page, here)");
    volatile uint64_t *wp = (volatile uint64_t *)(uintptr_t)va;
    for (unsigned k = 0; k < J_PAGE / 8u; k++) {
        wp[k] = 0;
    }
    r->w = (struct work *)(uintptr_t)va;
    must(j_map(r->frame, r->task, WORK_VA / J_PAGE, J_MAP_W), "map (a work page)");
    return r->w;
}

/* Every capability of record i's dropped, its task destroyed if it is a
 * child (E_GONE if that happened already), and the root's view of its
 * work page unmapped. */
static __attribute__((unused)) void rec_free(unsigned i)
{
    struct rec *r = &recs[i];
    if (!r->live) {
        return;
    }
    if (r->kid) {
        (void)j_task_destroy(r->task);
        (void)j_cap_delete(r->thread);
        (void)j_cap_delete(r->sc);
        (void)j_cap_delete(r->thread2);
        (void)j_cap_delete(r->sc2);
    }
    (void)j_cap_delete(r->task);
    if (r->frame != 0) {
        must(j_unmap(J_ROOT_SELF, (HOST_VIEW_VA + i * J_PAGE) / J_PAGE), "unmap (a work page)");
    }
    r->live = 0;
}

/* MMC_RQ_START from the task of record p: its child's task and thread
 * capabilities came in rx0 and rx1. 0, or 1 if refused. */
static uint64_t start_kid(unsigned p, const struct j_msg *m, unsigned *kid)
{
    if (J_CAPS_COUNT(m->caps) != 2 || J_CAPS_SLOT(m->caps, 0) != rx0 ||
        J_CAPS_SLOT(m->caps, 1) != rx1) {
        (void)j_cap_delete(rx0);
        (void)j_cap_delete(rx1);
        return 1;
    }
    unsigned i = rec_new();
    struct rec *r = &recs[i];
    r->kid = 1;
    r->parent = p;
    must(j_cap_derive(rx0, r->task, J_R_ALL), "cap_derive (a child)");
    must(j_cap_derive(rx1, r->thread, J_R_ALL), "cap_derive (a child's thread)");
    must(j_cap_delete(rx0), "cap_delete (rx)");
    must(j_cap_delete(rx1), "cap_delete (rx)");
    uint64_t entry = ld_image(1, r->task);
    struct work *w = rec_page(i);
    w->role = m->r[1];
    must(j_map(r->frame, recs[p].task, MMC_KID_VA(m->r[2]) / J_PAGE, J_MAP_W),
         "map (a child's work page, in its parent)");
    give_root_cap(i);
#ifdef HOST_HELPER_TOP
    ld_stack(r->task, HOST_HELPER_TOP);
    must(j_thread_spawn(r->task, r->thread2, entry, HOST_HELPER_TOP), "thread_spawn (a helper)");
    must(j_sc_create(r->sc2, HOST_PRIO, 10 * J_MS, 10 * J_MS), "sc_create (a helper)");
    must(j_sc_bind(r->sc2, r->thread2), "sc_bind (a helper)");
#else
    (void)entry;
#endif
    must(j_sc_create(r->sc, HOST_PRIO, 10 * J_MS, 10 * J_MS), "sc_create (a child)");
    must(j_sc_bind(r->sc, r->thread), "sc_bind (a child)");
    *kid = i;
    return 0;
}

/* One request to the root, if one comes within ns: 1 if one came. A
 * child started is in *kid (NRECS if none). */
static __attribute__((unused)) int host_serve(uint64_t ns, unsigned *kid)
{
    *kid = NRECS;
    struct j_msg m = {0};
    if (j_recv(root_ep, &m, ns) != J_OK) {
        return 0;
    }
    struct j_msg out = {0};
    out.r[0] = m.badge >= 1 && m.badge <= NRECS && recs[m.badge - 1u].live &&
                       m.r[0] == MMC_RQ_START
                   ? start_kid((unsigned)(m.badge - 1u), &m, kid)
                   : 1;
    (void)j_reply(&out);
    return 1;
}

#endif /* JOIST_USER_MMHOST_H */
