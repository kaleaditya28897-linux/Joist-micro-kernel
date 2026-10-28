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
    MMC_OUTSIDE     /* a read outside every region: mm ends the thread */
};
#define MMC_VA 0x08000000ull /* where it asks for memory: a last-level table's 512 pages */

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
