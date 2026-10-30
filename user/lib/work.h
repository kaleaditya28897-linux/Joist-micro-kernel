/* work.h - what the scheduler test's root task (schedroot) and its workers
 * share (Phase 3 step 5): one page each, mapped writable in both, through
 * which the root tells a worker what to be and reads how far it has got. */
#ifndef JOIST_USER_WORK_H
#define JOIST_USER_WORK_H

#include <stdint.h>

#define WORK_VA 0x30000000ull /* a worker's page, in the worker */

enum {
    WORK_SPIN = 1, /* count for ever, never calling the kernel */
    WORK_SERVE,    /* on the endpoint in slot 0: arg counts per request, then the reply */
    WORK_CLIENT,   /* call the endpoint in slot 0 for ever; count is the calls answered */
    WORK_CALLS,    /* make the cheapest system call for ever: nearly all its time is the kernel's */
    WORK_TICK      /* sleep arg ns again and again (a Recv on slot 0 that times out); count the
                    * wake-ups, and keep in aux the longest time from one to the next (TSC) */
};

/* The fault test's program (faulter; Phase 3 step 6). Each sets count to 1,
 * does its thing, and sets count to 2 if it gets past it. */
enum {
    FAULT_UD = 10, /* an invalid instruction (ud2, two bytes) */
    FAULT_PAGE,    /* writes arg to the unmapped address FAULT_VA, then reads it back into arg */
    FAULT_PAGE2,   /* writes to FAULT_VA; waits for aux to be set; writes there again */
    FAULT_GP,      /* a privileged instruction (hlt) */
    FAULT_DIV,     /* a division by zero */
    FAULT_TLS,     /* reads the word at its TLS base into arg */
    FAULT_BLOCK,   /* a Recv for ever on the endpoint in slot 0; arg is its result */
    FAULT_REGS,    /* puts the registers it started with (RDI, RSI) into arg */
    FAULT_BENCH    /* touches arg fresh pages from FAULT_BENCH_VA, one fault each, timing
                    * each round trip: aux the median, arg the fastest (TSC cycles) */
};
#define FAULT_VA 0x50000000ull
#define FAULT_BENCH_VA 0x60000000ull /* a last-level table's 512 pages (2 MiB aligned) */
#define FAULT_BENCH_MAX 240u
/* FAULT_BENCH also leaves, after the struct in its page, each fault's TSC
 * just before the touch (t0) and just after (t1), for its handler to break
 * the round trip down with its own stamps. */
#define FAULT_BENCH_T0(w) ((volatile uint64_t *)((volatile char *)(w) + 64))
#define FAULT_BENCH_T1(w) (FAULT_BENCH_T0(w) + FAULT_BENCH_MAX)

/* The memory manager's test program (mmclient; Phase 4 step 6, part D),
 * a client of mm through MM_SLOT. Each sets count to 1, does its thing, and
 * sets count to 2 if it gets past it; arg is 0, or the number of the first
 * step that went wrong. */
enum {
    MMC_BASIC = 20, /* map anonymous memory, read, write, protect, unmap, map again */
    MMC_RO_WRITE,   /* a write to a page mm made read-only: mm ends the thread */
    MMC_OUTSIDE,    /* a read outside every region: mm ends the thread */
    MMC_FORK,       /* clone; parent and child write the same pages, each seeing its own */
    MMC_CLONE_FULL, /* clones refused, and undone: mm's table pages spent, the child's too few */
    MMC_SHARE,      /* SHARE refused without consent, then shared, and kept across a clone */
    MMC_CHILD,      /* a child: does what its parent posts in its work page (MMC_C_*) */
    MMC_BENCH,      /* mm's costs, timed (TSC): faults, a request, protect, SHARE, clones of
                     * up to arg pages; the medians and fastest in MMC_W from MMC_B (MMC_B_*) */
    MMC_ORPHAN,     /* a fork cancelled; then it exits with one child confirmed and one
                     * pending: mm destroys the pending one with it, serves the other */
    MMC_GUARD,      /* MM_NONE (0024): pages made of no access and back keep what they held;
                     * a guard can't be mapped over, and a touch ends the thread */
    MMC_QUOTA       /* a child spends its quota (MM_CHILD_QUOTA), then forks: its child's
                     * thread refused, the clone cancelled, mm forgets the child */
};
#define MMC_B 100u
enum {
    MMC_B_ZERO_W, MMC_B_ZERO_R, MMC_B_COPY, MMC_B_UPGRADE, MMC_B_REQ, MMC_B_PROT_RO, MMC_B_PROT_W,
    MMC_B_SHARE,
    MMC_B_CLONE /* then one for each size in MMC_BENCH_SIZES */
};
#define MMC_BENCH_SIZES 256u, 4096u, 65536u /* 1, 16 and 256 MiB */
#define MMC_BENCH_VA 0x40000000ull          /* where the clones' pages are (the second GiB) */
#define MMC_VA 0x08000000ull /* where it asks for memory: a last-level table's 512 pages */

/* How mmclient makes a child (MMC_FORK, MMC_SHARE): CLONE, its capabilities
 * copied, its thread spawned, then a call to the root (in MMC_ROOT_SLOT)
 * with the child's task and thread capabilities: the root loads the
 * program's image into it, gives it a work page, mapped in the parent too
 * at MMC_KID_VA(n) (n is the parent's number for it), and a context. Then
 * the fork is confirmed to mm (MM_REQ_CLONE_DONE). */
#define MMC_ROOT_SLOT 12u /* a client's capability to the root (each its own badge) */
#define MMC_COPY_END  12u /* a child gets copies of its parent's slots below this */
#define MMC_KID_SLOT  16u /* the parent's capabilities to its children, from here ... */
#define MMC_KID_THREAD 48u /* ... and to their threads, from here */
#define MMC_KIDS      24u
#define MMC_STACK     0x400000ull /* a child's stack top: loader.h's CHILD_LO, as ld_image
                                   * gives it */
#define MMC_KID_VA(n) (WORK_VA + ((uint64_t)(n) + 1u) * 0x1000ull)
#define MMC_FULL_REGIONS 8u /* MMC_CLONE_FULL's pages, a table's span apart: more tables
                             * than a child is lent */
enum { MMC_RQ_START = 1 }; /* to the root: r[1] the child's role, r[2] n; caps its task, thread */

/* A child's commands: its parent writes the arguments and the command,
 * then bumps SEQ; the child does it, puts its result in RES and sets ACK
 * to SEQ. Words of the work page after struct work. */
#define MMC_W(w, i) (((volatile uint64_t *)(w))[8u + (i)])
enum { MMC_SEQ, MMC_ACK, MMC_CMD, MMC_A0, MMC_A1, MMC_A2, MMC_RES };
enum {
    MMC_C_EXIT = 1, /* count = 2, and exit */
    MMC_C_FILL,     /* pages a0 to a0 + a1 of MMC_VA written with tag a2 */
    MMC_C_CHECK,    /* -> how many words of those pages aren't tag a2's */
    MMC_C_ACCEPT,   /* -> MM_REQ_SHARE_ACCEPT's result: from id a0, at page a1, a2 pages */
    MMC_C_ID,       /* -> its id (MM_REQ_ID) */
    MMC_C_READ,     /* -> word a1 of page a0 of MMC_VA */
    MMC_C_POKE,     /* word a1 of page a0 of MMC_VA set to a2 */
    MMC_C_UNMAP,    /* -> MM_REQ_UNMAP's result: pages a0 (a va page) to a0 + a1 */
    MMC_C_DONE,     /* -> MM_REQ_CLONE_DONE's result: child id a0, how a1 */
    MMC_C_QUOTA     /* -> objects its quota paid for before it ran out, after a fork
                     * that then failed for want of quota was cancelled; or 1000 + a step */
};

struct work {
    volatile uint64_t role;
    volatile uint64_t arg;
    volatile uint64_t count;
    volatile uint64_t aux;
};

/* The time stamp counter: a clock user mode can read for itself. */
static inline uint64_t work_tsc(void)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* The one loop every count comes from, never inlined: within a program
 * every count is made by the same instructions, so counts compare as time.
 * (Inlined, the spinner's and the server's copies were compiled apart and
 * ran at rates a factor of two apart on a real CPU, though alike under
 * emulation.) */
static __attribute__((noinline, unused)) void work_spin(struct work *w, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        w->count = w->count + 1u;
    }
}

#endif /* JOIST_USER_WORK_H */
