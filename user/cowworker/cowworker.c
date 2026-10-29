/* cowworker - every task of cowstress's fork trees (Phase 4's exit test;
 * user/cowstress). A task mm serves, with two threads: the main one does
 * what the root posts in the work page (cow.h: write, verify, fork, exit),
 * and has the helper write alongside it, the same pages, its own slots, so
 * that two threads of one task take copy-on-write faults on the same pages
 * at once. Both start here; the helper's stack is the lower one. */
#include "../lib/joist.h"
#include "mm_proto.h"
#include "mmlib.h"
#include "../lib/cow.h"

void _start(void);

static struct work *const w = (struct work *)(uintptr_t)WORK_VA;

static uint64_t req(uint64_t r0, uint64_t a, uint64_t b, uint64_t c)
{
    struct j_msg m = {.r = {r0, a, b, c}};
    return j_call(MM_SLOT, &m, J_FOREVER) != J_OK ? 0xffu : m.r[0];
}

/* n writes to slot `slot` of pages chosen from seed, with one tag; the
 * main thread counts them in PROG. */
static void writes(uint64_t n, uint64_t seed, uint64_t tag, uint64_t slot)
{
    for (uint64_t k = 0; k < n; k++) {
        cow_write(cow_next(&seed) % COW_PAGES, slot, tag);
        if (slot == 0) {
            MMC_W(w, COW_PROG) = k + 1u;
        }
    }
}

static void helper(void)
{
    uint64_t seen = 0;
    for (;;) {
        while (MMC_W(w, COW_HSEQ) == seen) {
            (void)j_yield();
        }
        seen = MMC_W(w, COW_HSEQ);
        if (MMC_W(w, COW_HEXIT)) {
            MMC_W(w, COW_HACK) = seen;
            return;
        }
        writes(MMC_W(w, COW_HN), MMC_W(w, COW_HSEED),
               cow_tag(w->arg, MMC_W(w, COW_HGEN), 1), 1);
        MMC_W(w, COW_HACK) = seen;
    }
}

static uint64_t hseq;

static void helper_go(void)
{
    MMC_W(w, COW_HSEQ) = ++hseq;
}

static void helper_wait(void)
{
    while (MMC_W(w, COW_HACK) != hseq) {
        (void)j_yield();
    }
}

static unsigned nkids;

/* A child: CLONE, the capabilities below MMC_COPY_END copied, its thread
 * spawned, the root asked to start it, then the fork confirmed to mm. A
 * kill before the confirmation takes the child with this task. */
static uint64_t fork_kid(void)
{
    if (nkids == MMC_KIDS) {
        return COW_E_REFUSED;
    }
    uint64_t task = MMC_KID_SLOT + nkids;
    uint64_t thread = MMC_KID_THREAD + nkids;
    if (j_recv_slots(task, J_SLOT_NONE, 1) != J_OK) {
        return COW_E_FAILED | 1u << 8;
    }
    MMC_W(w, COW_INCLONE) = 1;
    struct j_msg m = {.r = {MM_REQ_CLONE, 0, 0, 0}};
    uint64_t e = j_call(MM_SLOT, &m, J_FOREVER);
    MMC_W(w, COW_INCLONE) = 2;
    if (e != J_OK) {
        return COW_E_FAILED | 2u << 8 | e << 16;
    }
    if (m.r[0] == MM_E_NOMEM) {
        return COW_E_REFUSED; /* mm is full */
    }
    if (m.r[0] != MM_OK || J_CAPS_COUNT(m.caps) != 1 || J_CAPS_SLOT(m.caps, 0) != task) {
        return COW_E_FAILED | 3u << 8 | m.r[0] << 16;
    }
    uint64_t id = m.r[1];
    uint64_t first = 0;
    while (first < MMC_COPY_END) {
        uint64_t done;
        uint64_t skipped;
        e = j_cap_copy_batch(task, first, MMC_COPY_END - first, J_R_ALL, 0, &done, &skipped);
        if (e == J_E_OCCUPIED && first + done == MM_SLOT) {
            first += done + 1u;
        } else if (e == J_OK) {
            first += done;
        } else {
            return COW_E_FAILED | 4u << 8 | e << 16;
        }
    }
    e = j_thread_spawn(task, thread, (uint64_t)(uintptr_t)_start, MMC_STACK);
    if (e != J_OK) {
        return COW_E_FAILED | 5u << 8 | e << 16;
    }
    m = (struct j_msg){.r = {MMC_RQ_START, 0, nkids, 0},
                       .caps = 2u | task << 2 | (uint64_t)J_R_ALL << 18 | thread << 22 |
                               (uint64_t)J_R_ALL << 38};
    nkids++;
    if (j_call(MMC_ROOT_SLOT, &m, J_FOREVER) != J_OK || m.r[0] != 0) {
        return COW_E_FAILED | 6u << 8;
    }
    m = (struct j_msg){.r = {MM_REQ_CLONE_DONE, id, MM_CLONE_KEEP, 0}};
    if (j_call(MM_SLOT, &m, J_FOREVER) != J_OK || m.r[0] != MM_OK) {
        return COW_E_FAILED | 7u << 8 | m.r[0] << 16;
    }
    /* The root has its own: this one would hold a unit of this task's
     * quota (it paid for the thread) for as long as this task lives. */
    (void)j_cap_delete(thread);
    return 0;
}

static void serve(void)
{
    /* Its pages (a child has its parent's region already, and is refused). */
    (void)req(MM_REQ_MAP_ANON, COW_VA / J_PAGE, COW_PAGES, MM_W);
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
        case COW_C_WRITE:
            MMC_W(w, COW_PROG) = 0;
            MMC_W(w, COW_HN) = a0;
            MMC_W(w, COW_HSEED) = COW_SEED2(a1);
            MMC_W(w, COW_HGEN) = a2;
            helper_go();
            writes(a0, a1, cow_tag(w->arg, a2, 0), 0);
            helper_wait();
            break;
        case COW_C_VERIFY:
            for (uint64_t p = 0; p < COW_PAGES; p++) {
                for (uint64_t s = 0; s < COW_SLOTS; s++) {
                    uint64_t t = cow_read(p, s);
                    COW_OBS(w, p, s) = t;
                    res += t == COW_BAD;
                }
            }
            break;
        case COW_C_FORK:
            res = fork_kid();
            break;
        case COW_C_EXIT:
            MMC_W(w, COW_HEXIT) = 1;
            helper_go();
            helper_wait();
            MMC_W(w, MMC_ACK) = seen;
            return;
        default:
            res = 0xbad;
            break;
        }
        MMC_W(w, MMC_RES) = res;
        MMC_W(w, MMC_ACK) = seen;
    }
}

void _start(void)
{
    uint64_t here = 0;
    if ((uint64_t)(uintptr_t)&here < COW_HELPER_TOP) {
        helper();
        j_exit(0);
    }
    w->count = 1;
    serve();
    w->count = 2;
    j_exit(0);
}
