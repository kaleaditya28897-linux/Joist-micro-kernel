/* mmclient - a task the memory manager serves (Phase 4 step 6, part D;
 * decision 0023), run by mmroot. Its role is in its work page (work.h:
 * MMC_*); it reaches mm through its capability in MM_SLOT, and mm answers
 * its page faults. A parent role makes children with mm's CLONE (work.h
 * says how) and drives them, each running this program as MMC_CHILD,
 * through their work pages. */
#include "../lib/joist.h"
#include "mm_proto.h"
#include "mmlib.h"
#include "../lib/work.h"

void _start(void);

#define PAGES 64u
#define WORDS (J_PAGE / 8u)

/* A request to mm; its r[1] in *out if out isn't 0. */
static uint64_t req2(uint64_t r0, uint64_t a, uint64_t b, uint64_t c, uint64_t *out)
{
    struct j_msg m = {.r = {r0, a, b, c}};
    uint64_t e = j_call(MM_SLOT, &m, J_FOREVER);
    if (out != 0) {
        *out = m.r[1];
    }
    return e != J_OK ? 0xffu : m.r[0];
}

static uint64_t req(uint64_t r0, uint64_t a, uint64_t b, uint64_t c)
{
    return req2(r0, a, b, c, 0);
}

static volatile uint64_t *page(uint64_t i)
{
    return (volatile uint64_t *)(uintptr_t)(MMC_VA + i * J_PAGE);
}

/* The value it writes at word `w` of page `i`. */
static uint64_t val(uint64_t i, uint64_t w)
{
    return (i + 1u) << 32 | (w * 7u + 1u);
}

static uint64_t basic(void)
{
    uint64_t va = MMC_VA / J_PAGE;
    if (req(MM_REQ_MAP_ANON, va, PAGES, MM_W) != MM_OK) {
        return 1;
    }
    if (req(MM_REQ_MAP_ANON, va + 10u, 4, MM_W) == MM_OK) {
        return 2; /* overlaps the first */
    }
    if (req(MM_REQ_MAP_ANON, va + PAGES, 4, MM_W | MM_X) == MM_OK) {
        return 3; /* W^X */
    }
    /* Demand-zero: a read of fresh memory finds zeroes (and maps the pages
     * around it), a write maps one page. */
    if (page(0)[0] != 0 || page(5)[WORDS - 1u] != 0) {
        return 4;
    }
    for (uint64_t i = 0; i < PAGES; i++) {
        page(i)[0] = val(i, 0);
        page(i)[WORDS - 1u] = val(i, WORDS - 1u);
    }
    for (uint64_t i = 0; i < PAGES; i++) {
        if (page(i)[0] != val(i, 0) || page(i)[WORDS - 1u] != val(i, WORDS - 1u) ||
            page(i)[1] != 0) {
            return 5;
        }
    }
    /* Read-only: still readable. */
    if (req(MM_REQ_PROTECT, va, PAGES / 2u, 0) != MM_OK) {
        return 6;
    }
    for (uint64_t i = 0; i < PAGES / 2u; i++) {
        if (page(i)[0] != val(i, 0)) {
            return 7;
        }
    }
    /* Unmapped, then asked for again: fresh zeroes. */
    if (req(MM_REQ_UNMAP, va + PAGES / 2u, PAGES / 2u, 0) != MM_OK) {
        return 8;
    }
    if (req(MM_REQ_MAP_ANON, va + PAGES / 2u, PAGES / 2u, MM_W) != MM_OK) {
        return 9;
    }
    for (uint64_t i = PAGES / 2u; i < PAGES; i++) {
        if (page(i)[0] != 0) {
            return 10;
        }
        page(i)[0] = val(i, 0);
    }
    /* Not this task's to ask, or not well formed. */
    if (req(MM_REQ_ADOPT, MM_S_ROOT, 0, 0) != MM_E_STATE) {
        return 11;
    }
    if (req(MM_REQ_PROTECT, va, 1, 7) != MM_E_RANGE) {
        return 12;
    }
    if (req(MM_REQ_UNMAP, va + PAGES, 1, 0) == 0xffu) {
        return 13;
    }
    if (req(MM_REQ_SHARE, va, 1, MM_ID(0, 0)) != MM_E_STATE) {
        return 14; /* nobody accepted it */
    }
    /* What was written is still there. */
    for (uint64_t i = 0; i < PAGES; i++) {
        if (page(i)[0] != val(i, 0)) {
            return 15;
        }
    }
    return 0;
}

/* ---- children (MMC_FORK, MMC_CLONE_FULL, MMC_SHARE) ---- */

/* The value a page of MMC_VA holds at word w when written with tag. */
static uint64_t tagged(uint64_t tag, uint64_t i, uint64_t w)
{
    return tag << 40 | i << 12 | w;
}

/* Words 0, 1 and the last of pages [first, first + n) of MMC_VA. */
static void fill(uint64_t first, uint64_t n, uint64_t tag)
{
    for (uint64_t i = first; i < first + n; i++) {
        page(i)[0] = tagged(tag, i, 0);
        page(i)[1] = tagged(tag, i, 1);
        page(i)[WORDS - 1u] = tagged(tag, i, WORDS - 1u);
    }
}

/* How many of those words don't hold tag's values. */
static uint64_t check(uint64_t first, uint64_t n, uint64_t tag)
{
    uint64_t bad = 0;
    for (uint64_t i = first; i < first + n; i++) {
        bad += page(i)[0] != tagged(tag, i, 0);
        bad += page(i)[1] != tagged(tag, i, 1);
        bad += page(i)[WORDS - 1u] != tagged(tag, i, WORDS - 1u);
    }
    return bad;
}

/* MMC_CHILD: do what the parent posts, until told to exit. */
static void child(struct work *w)
{
    uint64_t seen = 0;
    for (;;) {
        while (MMC_W(w, MMC_SEQ) == seen) {
            (void)j_yield();
        }
        seen = MMC_W(w, MMC_SEQ);
        uint64_t a0 = MMC_W(w, MMC_A0);
        uint64_t a1 = MMC_W(w, MMC_A1);
        uint64_t a2 = MMC_W(w, MMC_A2);
        uint64_t res = 0;
        switch (MMC_W(w, MMC_CMD)) {
        case MMC_C_EXIT:
            MMC_W(w, MMC_ACK) = seen;
            return;
        case MMC_C_FILL:
            fill(a0, a1, a2);
            break;
        case MMC_C_CHECK:
            res = check(a0, a1, a2);
            break;
        case MMC_C_ACCEPT:
            res = req(MM_REQ_SHARE_ACCEPT, a0, a1, a2);
            break;
        case MMC_C_READ:
            res = page(a0)[a1];
            break;
        case MMC_C_POKE:
            page(a0)[a1] = a2;
            break;
        case MMC_C_UNMAP:
            res = req(MM_REQ_UNMAP, a0, a1, 0);
            break;
        case MMC_C_DONE:
            res = req(MM_REQ_CLONE_DONE, a0, a1, 0);
            break;
        case MMC_C_ID:
            if (req2(MM_REQ_ID, 0, 0, 0, &res) != MM_OK) {
                res = 0;
            }
            break;
        default:
            res = 0xbad;
            break;
        }
        MMC_W(w, MMC_RES) = res;
        MMC_W(w, MMC_ACK) = seen;
    }
}

struct kid {
    uint64_t task, thread, id, seq;
    struct work *w;
};
static struct kid kids[MMC_KIDS];
static unsigned nkids;

/* Post a command to child k and wait for its result. */
static uint64_t cmd(struct kid *k, uint64_t c, uint64_t a0, uint64_t a1, uint64_t a2)
{
    MMC_W(k->w, MMC_A0) = a0;
    MMC_W(k->w, MMC_A1) = a1;
    MMC_W(k->w, MMC_A2) = a2;
    MMC_W(k->w, MMC_CMD) = c;
    MMC_W(k->w, MMC_SEQ) = ++k->seq;
    while (MMC_W(k->w, MMC_ACK) != k->seq) {
        (void)j_yield();
    }
    return MMC_W(k->w, MMC_RES);
}

/* CLONE alone: a child with this task's memory, in kids[nkids] (its task
 * capability and id), but no thread. mm's result. */
static uint64_t clone_only(void)
{
    struct kid *k = &kids[nkids];
    k->task = MMC_KID_SLOT + nkids;
    k->thread = MMC_KID_THREAD + nkids;
    k->seq = 0;
    if (j_recv_slots(k->task, J_SLOT_NONE, 1) != J_OK) {
        return 0xfe;
    }
    struct j_msg m = {.r = {MM_REQ_CLONE, 0, 0, 0}};
    if (j_call(MM_SLOT, &m, J_FOREVER) != J_OK) {
        return 0xff;
    }
    if (m.r[0] != MM_OK) {
        return m.r[0];
    }
    if (J_CAPS_COUNT(m.caps) != 1 || J_CAPS_SLOT(m.caps, 0) != k->task) {
        return 0xfd; /* the child's capability didn't arrive */
    }
    k->id = m.r[1];
    nkids++;
    return MM_OK;
}

/* A child in `role`, running (work.h): 0, or what went wrong. */
static uint64_t fork_kid(uint64_t role)
{
    if (nkids == MMC_KIDS || clone_only() != MM_OK) {
        return 1;
    }
    struct kid *k = &kids[nkids - 1u];
    /* Its capabilities: the batch stops where the child has one already
     * (MM_SLOT, if this task's copy could be copied), and goes on after. */
    uint64_t first = 0;
    while (first < MMC_COPY_END) {
        uint64_t done;
        uint64_t skipped;
        uint64_t e = j_cap_copy_batch(k->task, first, MMC_COPY_END - first, J_R_ALL, 0, &done,
                                      &skipped);
        if (e == J_E_OCCUPIED && first + done == MM_SLOT) {
            first += done + 1u;
        } else if (e == J_OK) {
            first += done;
        } else {
            return 2;
        }
    }
    if (j_thread_spawn(k->task, k->thread, (uint64_t)(uintptr_t)_start, MMC_STACK) != J_OK) {
        return 3;
    }
    struct j_msg m = {.r = {MMC_RQ_START, role, nkids - 1u, 0},
                      .caps = 2u | (uint64_t)k->task << 2 | (uint64_t)J_R_ALL << 18 |
                              (uint64_t)k->thread << 22 | (uint64_t)J_R_ALL << 38};
    if (j_call(MMC_ROOT_SLOT, &m, J_FOREVER) != J_OK || m.r[0] != 0) {
        return 4;
    }
    /* It runs: the fork is done, and the child no longer goes with us. */
    if (req(MM_REQ_CLONE_DONE, k->id, MM_CLONE_KEEP, 0) != MM_OK) {
        return 5;
    }
    k->w = (struct work *)(uintptr_t)MMC_KID_VA(nkids - 1u);
    return 0;
}

/* Wait for the root to look (it sets aux to `mark` plus one). */
static void pause_for_root(struct work *w, uint64_t mark)
{
    w->aux = mark;
    while (w->aux == mark) {
        (void)j_yield();
    }
}

/* Destroy child k, finish its destruction, and have mm collect it. */
static uint64_t kill_kid(struct kid *k)
{
    if (j_task_destroy(k->task) != J_OK) {
        return 1;
    }
    int more = 1;
    for (unsigned i = 0; more && i < 1000u; i++) {
        if (j_continue(0, &more) != J_OK) {
            return 2;
        }
    }
    (void)j_cap_delete(k->task);
    (void)j_cap_delete(k->thread);
    uint64_t id;
    return req2(MM_REQ_ID, 0, 0, 0, &id) == MM_OK ? 0 : 3;
}

/* Its exit, waited for, then its destruction. */
static uint64_t end_kid(struct kid *k)
{
    (void)cmd(k, MMC_C_EXIT, 0, 0, 0);
    while (k->w->count != 2) {
        (void)j_yield();
    }
    return kill_kid(k);
}

#define FORK_PAGES 16u

/* MMC_FORK: parent and child write the same pages, each its own pattern,
 * and see only their own; the pages neither wrote are the old pattern in
 * both. Then the child goes and mm forgets it. */
static uint64_t fork_test(struct work *w)
{
    uint64_t va = MMC_VA / J_PAGE;
    if (req(MM_REQ_MAP_ANON, va, FORK_PAGES, MM_W) != MM_OK) {
        return 1;
    }
    fill(0, FORK_PAGES, 1);
    if (fork_kid(MMC_CHILD) != 0) {
        return 2;
    }
    struct kid *k = &kids[0];
    if (cmd(k, MMC_C_CHECK, 0, FORK_PAGES, 1) != 0) {
        return 3; /* the child doesn't see what was there at the clone */
    }
    /* Pages 0-3 written by the parent first, 4-7 by the child first; each
     * looks straight after the other wrote. */
    fill(0, 4, 2);
    if (cmd(k, MMC_C_CHECK, 0, 4, 1) != 0) {
        return 8; /* the child sees the parent's writes */
    }
    (void)cmd(k, MMC_C_FILL, 0, 8, 3);
    if (check(4, 4, 1) != 0) {
        return 9; /* the parent sees the child's */
    }
    fill(4, 4, 2);
    if (check(0, 8, 2) != 0 || check(8, FORK_PAGES - 8u, 1) != 0) {
        return 4;
    }
    if (cmd(k, MMC_C_CHECK, 0, 8, 3) != 0 || cmd(k, MMC_C_CHECK, 8, FORK_PAGES - 8u, 1) != 0) {
        return 5;
    }
    pause_for_root(w, 1); /* two tasks, 16 + 8 pages */
    if (end_kid(k) != 0) {
        return 6;
    }
    if (check(0, 8, 2) != 0 || check(8, FORK_PAGES - 8u, 1) != 0) {
        return 7;
    }
    pause_for_root(w, 3); /* one task, 16 pages */
    return 0;
}


/* MMC_CLONE_FULL: clones until mm's table pages are spent, each refused
 * one undone (no task left over); and a clone refused part way, the
 * child's tables too few for this task's pages. */
static uint64_t full_test(struct work *w)
{
    uint64_t va = MMC_VA / J_PAGE;
    if (req(MM_REQ_MAP_ANON, va, 4, MM_W) != MM_OK) {
        return 1;
    }
    fill(0, 4, 1);
    uint64_t e;
    while ((e = clone_only()) == MM_OK) {
        if (nkids == MMC_KIDS) {
            return 2; /* never refused */
        }
    }
    if (e != MM_E_NOMEM) {
        return 3;
    }
    MMC_W(w, MMC_RES) = nkids;
    pause_for_root(w, 1); /* 1 + nkids tasks */
    if (clone_only() != MM_E_NOMEM) {
        return 4;
    }
    while (nkids > 0) {
        if (kill_kid(&kids[--nkids]) != 0) {
            return 5;
        }
    }
    if (clone_only() != MM_OK || kill_kid(&kids[--nkids]) != 0) {
        return 6; /* not given back */
    }
    /* One page in each of several tables' spans: the child's lent pages
     * can't hold their tables, and mm_clone fails part way. */
    for (uint64_t i = 1; i <= MMC_FULL_REGIONS; i++) {
        if (req(MM_REQ_MAP_ANON, va + i * MM_SPAN, 1, MM_W) != MM_OK) {
            return 7;
        }
        page(i * MM_SPAN)[0] = i;
    }
    if (clone_only() != MM_E_KERNEL) {
        return 8;
    }
    pause_for_root(w, 3); /* one task, 4 + 8 pages */
    for (uint64_t i = 1; i <= MMC_FULL_REGIONS; i++) {
        if (page(i * MM_SPAN)[0] != i) {
            return 9;
        }
        page(i * MM_SPAN)[0] = i + 1u; /* writable again (made read-only for the clone) */
        if (req(MM_REQ_UNMAP, va + i * MM_SPAN, 1, 0) != MM_OK) {
            return 10;
        }
    }
    if (clone_only() != MM_OK || kill_kid(&kids[--nkids]) != 0) {
        return 11;
    }
    return check(0, 4, 1) == 0 ? 0 : 12;
}

#define ORPHAN_PAGES 8u

/* MMC_ORPHAN: a fork cancelled, and confirmations refused (another task's
 * id, a stale one, twice, or by a child for its sibling); a pending child
 * destroyed by this task; then a child confirmed and one not, and this
 * task exits without a word: the root destroys it, and mm must destroy the
 * child still pending with it, and only that one. */
static uint64_t orphan_test(struct work *w)
{
    uint64_t me;
    if (req2(MM_REQ_ID, 0, 0, 0, &me) != MM_OK) {
        return 1;
    }
    if (req(MM_REQ_MAP_ANON, MMC_VA / J_PAGE, ORPHAN_PAGES, MM_W) != MM_OK) {
        return 2;
    }
    fill(0, ORPHAN_PAGES, 1);
    if (clone_only() != MM_OK) {
        return 3;
    }
    struct kid *a = &kids[nkids - 1u];
    if (req(MM_REQ_CLONE_DONE, me, MM_CLONE_KEEP, 0) != MM_E_STATE) {
        return 4; /* not a child of this task's */
    }
    if (req(MM_REQ_CLONE_DONE, MM_ID(MM_ID_TASK(a->id), MM_ID_GEN(a->id) + 1u), MM_CLONE_KEEP,
            0) != MM_E_STATE) {
        return 5; /* not a's generation */
    }
    if (req(MM_REQ_CLONE_DONE, a->id, 2, 0) != MM_E_RANGE) {
        return 6; /* neither keep nor cancel */
    }
    if (req(MM_REQ_CLONE_DONE, a->id, MM_CLONE_CANCEL, 0) != MM_OK) {
        return 7;
    }
    if (j_task_destroy(a->task) != J_E_GONE) {
        return 8; /* mm didn't destroy it */
    }
    if (req(MM_REQ_CLONE_DONE, a->id, MM_CLONE_CANCEL, 0) != MM_E_STATE) {
        return 9; /* gone already */
    }
    (void)j_cap_delete(a->task);
    nkids--;
    /* A pending child destroyed by this task itself: its index is free for
     * the next task mm takes, which must not be taken for this one's. */
    if (clone_only() != MM_OK || kill_kid(&kids[--nkids]) != 0) {
        return 10;
    }
    pause_for_root(w, 1); /* one task, 8 pages; the root adopts another */
    if (fork_kid(MMC_CHILD) != 0) {
        return 11;
    }
    struct kid *b = &kids[nkids - 1u];
    if (req(MM_REQ_CLONE_DONE, b->id, MM_CLONE_KEEP, 0) != MM_E_STATE) {
        return 12; /* confirmed already */
    }
    if (clone_only() != MM_OK) {
        return 13;
    }
    if (cmd(b, MMC_C_DONE, kids[nkids - 1u].id, MM_CLONE_KEEP, 0) != MM_E_STATE) {
        return 14; /* b confirmed its sibling: only the parent may */
    }
    pause_for_root(w, 3); /* three tasks, still 8 pages */
    return 0;
}

#define SH_VA   (MMC_VA / J_PAGE + 64u)  /* this task's pages to share */
#define SH_AT   (MMC_VA / J_PAGE + 128u) /* where the first child takes them */
#define SH2_VA  (MMC_VA / J_PAGE + 96u)  /* pages shared with the last child */
#define SH2_AT  (MMC_VA / J_PAGE + 160u)
#define SH_N    4u

/* MMC_SHARE: SHARE refused without the target's consent, with another
 * count, or to a stale id (the target destroyed, its index used again);
 * then shared both ways, and kept shared in a later child. */
static uint64_t share_test(struct work *w)
{
    uint64_t me;
    if (req2(MM_REQ_ID, 0, 0, 0, &me) != MM_OK || me == 0) {
        return 1;
    }
    if (req(MM_REQ_MAP_ANON, MMC_VA / J_PAGE, 8, MM_W) != MM_OK) {
        return 2;
    }
    fill(0, 8, 1);
    if (fork_kid(MMC_CHILD) != 0) {
        return 3;
    }
    struct kid *a = &kids[0];
    if (cmd(a, MMC_C_ID, 0, 0, 0) != a->id) {
        return 4; /* the child's id isn't what CLONE said */
    }
    /* Pages of this task's own, made after the clone (so a has none). */
    if (req(MM_REQ_MAP_ANON, SH_VA, SH_N, MM_W) != MM_OK) {
        return 5;
    }
    fill(64, SH_N, 4);
    if (req(MM_REQ_SHARE, SH_VA, SH_N, a->id) != MM_E_STATE) {
        return 6; /* a hasn't accepted */
    }
    if (cmd(a, MMC_C_ACCEPT, me, SH_AT, SH_N) != MM_OK) {
        return 7;
    }
    if (req(MM_REQ_SHARE, SH_VA, SH_N - 1u, a->id) != MM_E_STATE) {
        return 8; /* not the count a accepted */
    }
    if (req(MM_REQ_SHARE, SH_VA, SH_N, MM_ID(MM_ID_TASK(a->id), MM_ID_GEN(a->id) + 1u)) !=
        MM_E_STATE) {
        return 9; /* not a's generation */
    }
    if (req(MM_REQ_SHARE, SH_VA, SH_N, a->id) != MM_OK) {
        return 10;
    }
    if (req(MM_REQ_SHARE, SH_VA, SH_N, a->id) != MM_E_STATE) {
        return 11; /* the acceptance is used up */
    }
    /* Shared both ways (at other addresses in a): a sees what is there,
     * writes, and this task sees it; and the other way. */
    for (uint64_t i = 0; i < SH_N; i++) {
        if (cmd(a, MMC_C_READ, 128 + i, 0, 0) != tagged(4, 64 + i, 0)) {
            return 12;
        }
    }
    (void)cmd(a, MMC_C_POKE, 128, 0, 0x5a5a);
    if (page(64)[0] != 0x5a5a) {
        return 13;
    }
    page(65)[0] = 0x6b6b;
    if (cmd(a, MMC_C_READ, 129, 0, 0) != 0x6b6b) {
        return 14;
    }
    /* A later child: the shared pages are shared with it too (at the same
     * addresses), the private ones are its copy. */
    if (fork_kid(MMC_CHILD) != 0) {
        return 15;
    }
    struct kid *b = &kids[1];
    if (cmd(b, MMC_C_CHECK, 0, 8, 1) != 0 || cmd(b, MMC_C_READ, 64, 0, 0) != 0x5a5a ||
        cmd(b, MMC_C_READ, 65, 0, 0) != 0x6b6b || cmd(b, MMC_C_CHECK, 66, 2, 4) != 0) {
        return 16;
    }
    (void)cmd(b, MMC_C_POKE, 66, 0, 0x7c7c); /* shared: seen here and by a */
    (void)cmd(b, MMC_C_FILL, 0, 1, 7);       /* private: b's own */
    if (page(66)[0] != 0x7c7c || cmd(a, MMC_C_READ, 130, 0, 0) != 0x7c7c || check(0, 8, 1) != 0) {
        return 17;
    }
    if (cmd(a, MMC_C_CHECK, 0, 1, 1) != 0 || cmd(b, MMC_C_CHECK, 0, 1, 7) != 0) {
        return 18; /* a private page saw another's write */
    }
    page(67)[0] = 0x8d8d;
    if (cmd(b, MMC_C_READ, 67, 0, 0) != 0x8d8d || cmd(a, MMC_C_READ, 131, 0, 0) != 0x8d8d) {
        return 19;
    }
    pause_for_root(w, 1); /* three tasks */
    /* A stale id: c destroyed, d made with its index. */
    if (fork_kid(MMC_CHILD) != 0) {
        return 20;
    }
    struct kid *c = &kids[2];
    uint64_t cid = c->id;
    if (end_kid(c) != 0) {
        return 21;
    }
    if (fork_kid(MMC_CHILD) != 0) {
        return 22;
    }
    struct kid *d = &kids[3];
    if (MM_ID_TASK(d->id) != MM_ID_TASK(cid) || d->id == cid) {
        return 23; /* (the test needs the index used again) */
    }
    if (req(MM_REQ_MAP_ANON, SH2_VA, SH_N, MM_W) != MM_OK) {
        return 24;
    }
    fill(96, SH_N, 9);
    if (cmd(d, MMC_C_ACCEPT, me, SH2_AT, SH_N) != MM_OK) {
        return 25;
    }
    if (req(MM_REQ_SHARE, SH2_VA, SH_N, cid) != MM_E_STATE) {
        return 26; /* c's id, though d accepted */
    }
    if (req(MM_REQ_SHARE, SH2_VA, SH_N, d->id) != MM_OK) {
        return 27;
    }
    for (uint64_t i = 0; i < SH_N; i++) {
        if (cmd(d, MMC_C_READ, 160 + i, 0, 0) != tagged(9, 96 + i, 0)) {
            return 28;
        }
    }
    /* Used up: with the place free again, the same SHARE is refused. */
    if (cmd(d, MMC_C_UNMAP, SH2_AT, SH_N, 0) != MM_OK ||
        req(MM_REQ_SHARE, SH2_VA, SH_N, d->id) != MM_E_STATE) {
        return 31;
    }
    if (end_kid(a) != 0 || end_kid(b) != 0 || end_kid(d) != 0) {
        return 29;
    }
    /* Still shared pages, with nobody now: still this task's. */
    if (page(64)[0] != 0x5a5a || page(65)[0] != 0x6b6b || page(66)[0] != 0x7c7c ||
        page(67)[0] != 0x8d8d || check(96, SH_N, 9) != 0 || check(0, 8, 1) != 0) {
        return 30;
    }
    pause_for_root(w, 3); /* one task */
    return 0;
}

/* ---- the benchmark (MMC_BENCH) ---- */

#define BENCH_N 240u
static uint64_t smp[BENCH_N];

/* The median and the fastest of n samples, into result k. */
static void bench_put(struct work *w, unsigned k, unsigned n)
{
    for (unsigned i = 1; i < n; i++) { /* insertion sort: n is small */
        uint64_t x = smp[i];
        unsigned j = i;
        for (; j > 0 && smp[j - 1u] > x; j--) {
            smp[j] = smp[j - 1u];
        }
        smp[j] = x;
    }
    MMC_W(w, MMC_B + 2u * k) = n != 0 ? smp[n / 2u] : 0;
    MMC_W(w, MMC_B + 2u * k + 1u) = n != 0 ? smp[0] : 0;
}

/* A write to page i of MMC_VA, timed. */
static uint64_t touch_w(uint64_t i)
{
    uint64_t t0 = work_tsc();
    page(i)[0] = i + 1u;
    return work_tsc() - t0;
}

static uint64_t bench(struct work *w)
{
    uint64_t va = MMC_VA / J_PAGE;
    /* Demand-zero writes, then copies: each page written again while a
     * child (with no thread) holds a reference to it. */
    if (req(MM_REQ_MAP_ANON, va, BENCH_N, MM_W) != MM_OK) {
        return 1;
    }
    for (uint64_t i = 0; i < BENCH_N; i++) {
        smp[i] = touch_w(i);
    }
    bench_put(w, MMC_B_ZERO_W, BENCH_N);
    if (clone_only() != MM_OK) {
        return 2;
    }
    for (uint64_t i = 0; i < BENCH_N; i++) {
        smp[i] = touch_w(i);
    }
    bench_put(w, MMC_B_COPY, BENCH_N);
    if (kill_kid(&kids[--nkids]) != 0 || req(MM_REQ_UNMAP, va, BENCH_N, 0) != MM_OK) {
        return 3;
    }
    /* Upgrades: read-only after a clone, the child gone, the last
     * reference this task's. */
    if (req(MM_REQ_MAP_ANON, va, BENCH_N, MM_W) != MM_OK) {
        return 4;
    }
    for (uint64_t i = 0; i < BENCH_N; i++) {
        page(i)[0] = 1;
    }
    if (clone_only() != MM_OK || kill_kid(&kids[--nkids]) != 0) {
        return 5;
    }
    for (uint64_t i = 0; i < BENCH_N; i++) {
        smp[i] = touch_w(i);
    }
    bench_put(w, MMC_B_UPGRADE, BENCH_N);
    if (req(MM_REQ_UNMAP, va, BENCH_N, 0) != MM_OK) {
        return 6;
    }
    /* Demand-zero reads: each maps MM_AROUND pages, so one every
     * MM_AROUND pages faults, over a table's span mapped afresh each time. */
    unsigned n = 0;
    while (n < BENCH_N) {
        if (req(MM_REQ_MAP_ANON, va, MM_SPAN, MM_W) != MM_OK) {
            return 7;
        }
        for (uint64_t i = 0; i < MM_SPAN && n < BENCH_N; i += MM_AROUND) {
            uint64_t t0 = work_tsc();
            uint64_t v = page(i)[0];
            smp[n++] = work_tsc() - t0;
            if (v != 0) {
                return 8;
            }
        }
        if (req(MM_REQ_UNMAP, va, MM_SPAN, 0) != MM_OK) {
            return 9;
        }
    }
    bench_put(w, MMC_B_ZERO_R, BENCH_N);
    /* A request's round trip (MM_REQ_ID: the least mm does). */
    for (unsigned i = 0; i < BENCH_N; i++) {
        uint64_t id;
        uint64_t t0 = work_tsc();
        uint64_t e = req2(MM_REQ_ID, 0, 0, 0, &id);
        smp[i] = work_tsc() - t0;
        if (e != MM_OK) {
            return 10;
        }
    }
    bench_put(w, MMC_B_REQ, BENCH_N);
    /* protect over a table's span, every page mapped: read-only, then
     * writable again, each a request to mm and one kernel call. */
    if (req(MM_REQ_MAP_ANON, va, MM_SPAN, MM_W) != MM_OK) {
        return 11;
    }
    for (uint64_t i = 0; i < MM_SPAN; i += MM_AROUND) {
        (void)page(i)[0];
    }
    uint64_t ro[BENCH_N / 4u];
    for (unsigned i = 0; i < BENCH_N / 4u; i++) {
        uint64_t t0 = work_tsc();
        uint64_t e = req(MM_REQ_PROTECT, va, MM_SPAN, 0);
        uint64_t t1 = work_tsc();
        e |= req(MM_REQ_PROTECT, va, MM_SPAN, MM_W);
        smp[i] = work_tsc() - t1;
        ro[i] = t1 - t0;
        if (e != MM_OK) {
            return 12;
        }
    }
    bench_put(w, MMC_B_PROT_W, BENCH_N / 4u);
    for (unsigned i = 0; i < BENCH_N / 4u; i++) {
        smp[i] = ro[i];
    }
    bench_put(w, MMC_B_PROT_RO, BENCH_N / 4u);
    if (req(MM_REQ_UNMAP, va, MM_SPAN, 0) != MM_OK) {
        return 13;
    }
    /* Clones of each size up to arg pages, every page mapped (by reads),
     * the child destroyed between. */
    static const uint64_t sizes[] = {MMC_BENCH_SIZES};
    for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0] && sizes[k] <= w->arg; k++) {
        uint64_t cva = MMC_BENCH_VA / J_PAGE;
        if (req(MM_REQ_MAP_ANON, cva, sizes[k], MM_W) != MM_OK) {
            return 14;
        }
        for (uint64_t i = 0; i < sizes[k]; i += MM_AROUND) {
            (void)((volatile uint64_t *)(uintptr_t)((cva + i) * J_PAGE))[0];
        }
        unsigned times = sizes[k] >= 65536u ? 3u : sizes[k] >= 4096u ? 5u : 9u;
        for (unsigned i = 0; i < times; i++) {
            uint64_t t0 = work_tsc();
            uint64_t e = clone_only();
            smp[i] = work_tsc() - t0;
            if (e != MM_OK || kill_kid(&kids[--nkids]) != 0) {
                return 15;
            }
        }
        bench_put(w, MMC_B_CLONE + k, times);
        if (req(MM_REQ_UNMAP, cva, sizes[k], 0) != MM_OK) {
            return 16;
        }
    }
    return 0;
}

void _start(void)
{
    struct work *w = (struct work *)(uintptr_t)WORK_VA;
    w->count = 1;
    switch (w->role) {
    case MMC_BASIC:
        w->arg = basic();
        break;
    case MMC_RO_WRITE:
        if (req(MM_REQ_MAP_ANON, MMC_VA / J_PAGE, 4, MM_W) != MM_OK) {
            w->arg = 1;
            break;
        }
        page(0)[0] = 1;
        if (req(MM_REQ_PROTECT, MMC_VA / J_PAGE, 4, 0) != MM_OK) {
            w->arg = 2;
            break;
        }
        w->aux = 1;
        page(0)[0] = 2; /* mm ends the thread here */
        w->arg = 3;
        break;
    case MMC_OUTSIDE:
        w->aux = 1;
        w->arg = page(PAGES * 4u)[0]; /* no region: mm ends the thread */
        w->arg = 3;
        break;
    case MMC_FORK:
        w->arg = fork_test(w);
        break;
    case MMC_CLONE_FULL:
        w->arg = full_test(w);
        break;
    case MMC_SHARE:
        w->arg = share_test(w);
        break;
    case MMC_ORPHAN:
        w->arg = orphan_test(w);
        break;
    case MMC_CHILD:
        child(w);
        break;
    case MMC_BENCH:
        w->aux = bench(w);
        break;
    default:
        w->arg = 99;
        break;
    }
    w->count = 2;
    j_exit(0);
}
