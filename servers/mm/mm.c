/* mm.c - the memory manager (decision 0023; Phase 4 step 6, part D).
 *
 * A passive server: one thread with no scheduling context of its own,
 * answering on its endpoint (MM_S_EP) with ReplyRecv, on the time each
 * caller lends it. The logic is mmlib's; this is the loop around it and the
 * kernel calls mmlib makes (struct mm_kops).
 *
 * The root task fills mm's first slots (mm_proto.h), starts its thread with
 * a context, makes the first call (MM_REQ_INIT) and takes the context away:
 * from then on mm runs only when called. The root then hands it tasks
 * (MM_REQ_ADOPT). Each gets its own badged capability to mm in MM_SLOT,
 * which is also its fault handler, and a run of pages for its tables.
 *
 * A task gone: when its slots are swept, the last capability with its badge
 * goes and the kernel queues the badge on mm's endpoint (no-senders, bound
 * by the root). Being passive, mm can't be woken to collect it; it collects
 * whatever is queued at the start of every message it gets, before looking
 * at it. A badge is only ever used again once collected (mm_task_add reuses
 * a task's index only after mm_task_gone), so a late collection never
 * mistakes a new task for an old one. */
#include "../../user/lib/joist.h"
#include "mm_proto.h"
#include "mmlib.h"

void _start(void);

static struct mm M;
static int ready;
static uint64_t tables_left;              /* pages left to lend in MM_S_TABLES */
static uint64_t next_slot = MM_S_FREE;    /* mm's own slots, up to MM_S_ROOT */
static uint64_t table_run[MM_MAX_TASKS];  /* the run lent to the task with that index */

/* ---- the kernel, for mmlib ---- */

static int k_map(void *ctx, uint32_t frame, uint64_t off, uint64_t n, uint32_t task, uint64_t va,
                 unsigned perms)
{
    (void)ctx;
    return j_map_run(frame, off, n, task, va, perms) == J_OK ? 0 : 1;
}

static int k_map_replace(void *ctx, uint32_t frame, uint64_t off, uint32_t task, uint64_t va,
                         unsigned perms, uint32_t old_frame, uint64_t old_off)
{
    (void)ctx;
    return j_map_replace(frame, off, 1, task, va, perms, old_frame, old_off) == J_OK ? 0 : 1;
}

static int k_unmap(void *ctx, uint32_t task, uint64_t va, uint64_t n)
{
    (void)ctx;
    return j_unmap_range(task, va, n) == J_OK ? 0 : 1;
}

static int k_protect(void *ctx, uint32_t task, uint64_t va, uint64_t n, unsigned perms)
{
    (void)ctx;
    return j_protect(task, va, n, perms) == J_OK ? 0 : 1;
}

/* mm's one frame, mapped at MM_POOL_VA by init(). */
static uint8_t *k_page(void *ctx, uint32_t frame, uint64_t off)
{
    (void)ctx;
    (void)frame;
    return (uint8_t *)(uintptr_t)(MM_POOL_VA + off * J_PAGE);
}

static const struct mm_kops kops = {
    .ctx = 0,
    .map = k_map,
    .map_replace = k_map_replace,
    .unmap = k_unmap,
    .protect = k_protect,
    .page = k_page,
};

/* ---- the root's requests ---- */

/* Map the pool into mm's own space, and take it and the table pages on. */
static uint64_t init(void)
{
    if (ready) {
        return MM_E_STATE;
    }
    uint64_t n = j_frame_pages(MM_S_POOL);
    if (n == 0 || n > MM_PAGES) {
        return MM_E_RANGE;
    }
    for (uint64_t off = 0; off < n; off += MM_SPAN) {
        uint64_t k = n - off < MM_SPAN ? n - off : MM_SPAN;
        if (j_map_run(MM_S_POOL, off, k, MM_S_SELF, MM_POOL_VA / J_PAGE + off, J_MAP_W) != J_OK) {
            return MM_E_KERNEL;
        }
    }
    mm_init(&M, &kops);
    if (mm_add_frame(&M, MM_S_POOL, n) != MM_OK) {
        return MM_E_NOMEM;
    }
    tables_left = j_frame_pages(MM_S_TABLES);
    ready = 1;
    return MM_OK;
}

/* A run of MM_TABLE_PAGES pages split off the end of MM_S_TABLES, in a slot
 * of mm's own; 0 if none is left. */
static uint64_t table_run_new(void)
{
    if (tables_left <= MM_TABLE_PAGES || next_slot >= MM_S_ROOT) {
        return 0;
    }
    uint64_t run = next_slot;
    if (j_frame_split(MM_S_TABLES, tables_left - MM_TABLE_PAGES, run) != J_OK) {
        return 0;
    }
    next_slot++;
    tables_left -= MM_TABLE_PAGES;
    return run;
}

static void collect(void);

/* Serve the task the root put a task capability for in `slot`: its badge in
 * *badge. */
static uint64_t adopt(uint64_t slot, uint64_t *badge)
{
    if (slot < MM_S_ROOT || slot >= 128u) {
        return MM_E_RANGE;
    }
    uint32_t t;
    int e = mm_task_add(&M, (uint32_t)slot, &t);
    if (e != MM_OK) {
        return (uint64_t)e;
    }
    /* Its tables first: this also checks slot holds a task with CONTROL,
     * before anything is minted for it. A run lent to a task that had this
     * index before is used again if the kernel lets it go with that task's
     * space. */
    uint64_t run = table_run[t];
    if (run == 0 || j_space_donate(slot, run) != J_OK) {
        run = table_run_new();
        if (run == 0) {
            mm_task_gone(&M, t);
            return MM_E_NOMEM;
        }
        table_run[t] = run;
        if (j_space_donate(slot, run) != J_OK) {
            mm_task_gone(&M, t);
            return MM_E_KERNEL;
        }
    }
    if (j_cap_mint(MM_S_EP, MM_S_TMP, J_R_SEND | J_R_GRANT, t + 1u) != J_OK) {
        mm_task_gone(&M, t);
        return MM_E_NOMEM; /* the no-senders accounts are full */
    }
    uint64_t g = j_cap_give(slot, MM_S_TMP, MM_SLOT, J_R_SEND);
    (void)j_cap_delete(MM_S_TMP);
    if (g != J_OK) {
        /* The badge's only sender went with MM_S_TMP: its firing is queued
         * now, and collecting it forgets the task (and drops mm's capability
         * to it) before its index can be used again. */
        collect();
        return MM_E_KERNEL;
    }
    if (j_task_set_fault_flags(slot, MM_SLOT, J_FAULT_NO_THREAD_CAP) != J_OK) {
        /* Can't happen once space_donate took the capability: it stays
         * served, and goes when its badge does. */
        return MM_E_KERNEL;
    }
    *badge = t + 1u;
    return MM_OK;
}

static uint64_t status(struct j_msg *out)
{
    uint64_t n = 0;
    for (uint32_t t = 0; t < MM_MAX_TASKS; t++) {
        n += M.task[t].live;
    }
    char msg[128];
    unsigned bad = ready ? mm_check(&M, msg, sizeof msg) : 0;
    if (bad != 0) {
        j_puts("mm: ");
        j_puts(msg);
        j_puts("\n");
    }
    out->r[1] = n;
    out->r[2] = M.nfree;
    out->r[3] = bad;
    return MM_OK;
}

/* ---- tasks gone ---- */

static void collect(void)
{
    uint64_t b;
    while (j_nosenders_next(MM_S_EP, &b) == J_OK && b != 0) {
        if (b > MM_MAX_TASKS || !M.task[b - 1u].live) {
            continue; /* not a task's (the root's never goes) */
        }
        uint32_t t = (uint32_t)(b - 1u);
        (void)j_cap_delete(M.task[t].slot);
        mm_task_gone(&M, t);
    }
}

/* ---- the loop ---- */

static void handle(const struct j_msg *in, struct j_msg *out)
{
    uint64_t r0 = in->r[0];
    uint64_t b = in->badge;
    if (b == MM_ROOT_BADGE) {
        switch (r0) {
        case MM_REQ_INIT:
            out->r[0] = init();
            return;
        case MM_REQ_ADOPT:
            out->r[0] = ready ? adopt(in->r[1], &out->r[1]) : MM_E_STATE;
            return;
        case MM_REQ_STATUS:
            out->r[0] = status(out);
            return;
        default:
            out->r[0] = MM_E_RANGE;
            return;
        }
    }
    int served = ready && b != 0 && b <= MM_MAX_TASKS && M.task[b - 1u].live;
    if (r0 < MM_REQ_BASE) { /* a fault */
        int resume = served && J_FAULT_KIND(r0) == J_FAULT_PAGE &&
                     mm_fault(&M, (uint32_t)(b - 1u), in->r[1] / J_PAGE,
                              (unsigned)J_FAULT_ACCESS(r0)) != 0;
        out->r[0] = resume ? J_FAULT_RESUME : 1u;
        return;
    }
    if (!served) {
        out->r[0] = MM_E_STATE;
        return;
    }
    uint32_t t = (uint32_t)(b - 1u);
    if ((r0 == MM_REQ_MAP_ANON || r0 == MM_REQ_PROTECT) && in->r[3] > (MM_W | MM_X)) {
        out->r[0] = MM_E_RANGE;
        return;
    }
    switch (r0) {
    case MM_REQ_MAP_ANON:
        out->r[0] = (uint64_t)mm_map_anon(&M, t, in->r[1], in->r[2], (unsigned)in->r[3]);
        return;
    case MM_REQ_UNMAP:
        out->r[0] = (uint64_t)mm_unmap(&M, t, in->r[1], in->r[2]);
        return;
    case MM_REQ_PROTECT:
        out->r[0] = (uint64_t)mm_protect(&M, t, in->r[1], in->r[2], (unsigned)in->r[3]);
        return;
    default: /* CLONE and SHARE come with part D2; INIT and ADOPT are the root's */
        out->r[0] = MM_E_STATE;
        return;
    }
}

void _start(void)
{
    struct j_msg m = {0};
    uint64_t e = j_recv(MM_S_EP, &m, J_FOREVER);
    for (;;) {
        struct j_msg out = {0};
        if (e == J_OK) {
            collect();
            handle(&m, &out);
            m = out;
            e = j_reply_recv(MM_S_EP, &m, J_FOREVER);
        } else {
            m = out;
            e = j_recv(MM_S_EP, &m, J_FOREVER);
        }
    }
}
