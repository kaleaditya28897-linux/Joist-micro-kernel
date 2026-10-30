/* SPDX-License-Identifier: GPL-3.0-only */
/* ipcbench.c - the two ends of the IPC benchmark (Phase 3 step 8;
 * benchroot starts them, in one task or in two). What a thread is comes in
 * its start registers: RDI the role (0 the server, 1 the client, 2 a thread
 * that does nothing but make system calls, 3 a client for a fixed time: the
 * scaling test's, scaleroot), RSI the name of the measurement.
 *
 * The server answers every call on the endpoint in slot 0 with the same
 * message (Recv once, then ReplyRecv: the server loop).
 *
 * The client makes BENCH_WARMUP calls on slot 1, then BENCH_CALLS more,
 * timing each from before the call to after its return with the time stamp
 * counter (lfence; rdtsc at both ends, so the figure includes one such
 * pair: measured with every batch, as the ruler is, and taken off). The
 * times of a segment go into a histogram, which gives the segment's median
 * and 99th percentile; what is printed is the median over the segments of
 * each, with the worst segment's median beside it, and the least and
 * greatest single time. Then it tells benchroot on slot 2.
 *
 * Why segments: the machine this is measured on is shared, and for a few
 * tenths of a second now and then everything on it is a quarter slower.
 * A run long enough to straddle such a stretch, judged by its middle
 * segment, isn't moved by it; the worst segment shows that it happened.
 *
 * The unit. The time stamp counter runs at a fixed rate, but the CPU's own
 * clock doesn't: it moves between turbo steps with load and temperature,
 * by tens of per cent from one run to the next and within a run, and the
 * same work then takes a different number of counts. So every batch of
 * BENCH_BATCH calls is preceded by a ruler: a chain of BENCH_RULER
 * multiplications, each waiting for the one before, which takes three of
 * the CPU's cycles each on every x86 this runs on. Times are printed in
 * those cycles (counts times 3 * BENCH_RULER over the ruler's counts), and
 * the ruler's own length, least and greatest over the run, shows how far
 * the clock moved. Under emulation the unit means little; the raw counts
 * per cycle are printed for that. */
#include "../lib/joist.h"
#include "../lib/work.h"

/* BENCH_SEGMENTS segments of BENCH_CALLS calls each. */
#ifndef BENCH_CALLS
#define BENCH_CALLS 5000u
#endif
#define BENCH_SEGMENTS 20u
#define BENCH_WARMUP 2000u
#define BENCH_BATCH  500u
#define BENCH_RULER  4000u
#define BUCKETS      4096u

void _start(uint64_t role, uint64_t name, uint64_t from, uint64_t to);

#ifdef BENCH_LOCK_STATS
/* The lock study's kernel call (run.c, RUN_LOCK_STATS): op 0 counts the
 * kernel lock's acquisitions from TSC `from` to `to`, op 1 prints them.
 * Any other kernel refuses the number and nothing happens. */
static inline void lock_stats(uint64_t op, uint64_t from, uint64_t to)
{
    (void)j_sys4(99, op, from, to, 0);
}
#endif

static uint32_t hist[BUCKETS + 1u]; /* the last: everything longer */

static const char *const names[] = {
    "two address spaces, passive server",
    "one address space, passive server",
    "two address spaces, server with its own context",
    "two address spaces, passive server, every other CPU making system calls",
    "two address spaces, passive server, 4 KiB of IPC buffer each way",
};

static uint64_t buf[512] __attribute__((aligned(4096))); /* this thread's IPC buffer */
static unsigned words; /* buffer words each call carries (and the reply brings back) */

static __attribute__((noreturn)) void serve(void)
{
    struct j_msg m = {0};
    if (j_ipc_buffer(buf) != J_OK || j_recv(0, &m, J_FOREVER) != J_OK) {
        j_puts("ipcbench: recv failed\n");
        j_exit(1);
    }
    for (;;) {
        m.caps = J_WORDS_SET(J_WORDS(m.caps)); /* as many words back as came */
        if (j_reply_recv(0, &m, J_FOREVER) != J_OK) {
            j_puts("ipcbench: reply_recv failed\n");
            j_exit(1);
        }
    }
}

static uint64_t bench_name; /* RSI: which measurement this is */
static int nothing; /* measure a system call that does nothing instead */

static uint64_t once(struct j_msg *m)
{
    if (nothing) {
        uint64_t t0 = work_tsc();
        (void)j_cap_delete(J_SLOT_NONE); /* refused at once: the way in and out */
        return work_tsc() - t0;
    }
    m->caps = J_WORDS_SET(words);
    uint64_t t0 = work_tsc();
    uint64_t e = j_call(1, m, J_FOREVER);
    uint64_t t1 = work_tsc();
    if (e != J_OK) {
        j_puts("ipcbench: call failed\n");
        j_exit(1);
    }
    return t1 - t0;
}

/* The ruler: the counts 3 * BENCH_RULER CPU cycles take now (the least of
 * three goes, so an interrupt in one doesn't stretch it). */
static uint64_t ruler(void)
{
    uint64_t least = UINT64_MAX;
    for (unsigned go = 0; go < 3u; go++) {
        uint64_t x = 3;
        uint64_t n = BENCH_RULER;
        uint64_t t0 = work_tsc();
        __asm__ volatile("1: imulq $3, %0, %0\n\tdecq %1\n\tjnz 1b" : "+r"(x), "+r"(n) : : "cc");
        uint64_t d = work_tsc() - t0;
        least = d < least ? d : least;
    }
    return least;
}

/* The middle of n values (sorts them in place). */
static uint64_t middle(uint64_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint64_t x = v[i];
        unsigned k = i;
        for (; k > 0 && v[k - 1u] > x; k--) {
            v[k] = v[k - 1u];
        }
        v[k] = x;
    }
    return v[n / 2u];
}

/* The value below which `part` of `of` samples lie (the bucket's upper edge). */
static uint64_t quantile(uint64_t part, uint64_t of, uint64_t width)
{
    uint64_t want = (uint64_t)BENCH_CALLS * part / of;
    want = want == 0 ? 1 : want;
    uint64_t seen = 0;
    for (unsigned i = 0; i <= BUCKETS; i++) {
        seen += hist[i];
        if (seen >= want && seen != 0) {
            return (i + 1u) * width;
        }
    }
    return 0;
}

static void put(const char *what, uint64_t v)
{
    j_puts(what);
    j_putu(v);
}

/* The clock's own cost in counts, now: the least of 200 back-to-back
 * readings. */
static uint64_t clock_cost(void)
{
    uint64_t least = UINT64_MAX;
    for (unsigned i = 0; i < 200u; i++) {
        uint64_t t0 = work_tsc();
        uint64_t d = work_tsc() - t0;
        least = d < least ? d : least;
    }
    return least;
}

static void measure(const char *what, struct j_msg *mp)
{
#define m (*mp)
    const uint64_t cycles = 3u * BENCH_RULER;
    uint64_t least = UINT64_MAX;
    uint64_t clock = clock_cost();
    uint64_t r0 = ruler();
    for (unsigned i = 0; i < BENCH_WARMUP; i++) {
        uint64_t t = once(&m);
        uint64_t d = (t > clock ? t - clock : 0) * cycles / r0;
        least = d < least ? d : least;
    }
    /* Buckets of 4 cycles (up to 16384, and one for everything longer),
     * wider by powers of two if the warm-up's least time is past an eighth
     * of that: so the median and 99th percentile land inside the histogram
     * whatever is measured. (Until 2026-10-15 the width was fixed at 4, and
     * a measurement slower than 16384 cycles printed that edge as its
     * median: the P53's hand-off found it.) */
    uint64_t width = 4;
    while (least / width > BUCKETS / 8u) {
        width *= 2u;
    }
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;
    uint64_t ruler_min = UINT64_MAX;
    uint64_t ruler_max = 0;
    uint64_t clock_min = UINT64_MAX;
    uint64_t clock_max = 0;
    uint64_t r = 1;
    static uint64_t p50[BENCH_SEGMENTS];
    static uint64_t p99[BENCH_SEGMENTS];
    uint64_t worst = 0;
#ifdef BENCH_LOCK_STATS
    if (bench_name == 3) { /* the contended measurement: the kernel lock's figures for it */
        lock_stats(0, work_tsc(), UINT64_MAX);
    }
#endif
    for (unsigned seg = 0; seg < BENCH_SEGMENTS; seg++) {
        for (unsigned b = 0; b <= BUCKETS; b++) {
            hist[b] = 0;
        }
        for (unsigned i = 0; i < BENCH_CALLS; i++) {
            if (i % BENCH_BATCH == 0) {
                /* The ruler and the clock's own cost, both in counts, both
                 * again for every batch: counts per CPU cycle change with
                 * the CPU's speed, and the two must be taken at the same
                 * one. (Until 2026-10-15 the clock's cost was taken once,
                 * at the start, where the CPU can still be coming up to
                 * speed from idle: five times too high, taken off every
                 * sample after, it gave the do-nothing call two clusters
                 * of results 30 % apart.) */
                r = ruler();
                clock = clock_cost();
                ruler_min = r < ruler_min ? r : ruler_min;
                ruler_max = r > ruler_max ? r : ruler_max;
                clock_min = clock < clock_min ? clock : clock_min;
                clock_max = clock > clock_max ? clock : clock_max;
            }
            uint64_t t = once(&m);
            uint64_t d = (t > clock ? t - clock : 0) * cycles / r;
            min = d < min ? d : min;
            max = d > max ? d : max;
            uint64_t b = d / width;
            hist[b < BUCKETS ? b : BUCKETS]++;
        }
        p50[seg] = quantile(1, 2, width);
        p99[seg] = quantile(99, 100, width);
        worst = p50[seg] > worst ? p50[seg] : worst;
    }
#ifdef BENCH_LOCK_STATS
    if (bench_name == 3) {
        lock_stats(1, 0, 0);
    }
#endif
    j_puts("ipcbench: ");
    j_puts(what);
    put(": n=", BENCH_SEGMENTS);
    put("x", BENCH_CALLS);
    put(" min=", min);
    put(" p50=", middle(p50, BENCH_SEGMENTS));
    put(" p99=", middle(p99, BENCH_SEGMENTS));
    put(" worst-p50=", worst);
    put(" max=", max);
    put(" cycles (bucket ", width);
    put("; ", cycles);
    put(" cycles took ", ruler_min);
    put(" to ", ruler_max);
    put(" counts; clock ", clock_min);
    put(" to ", clock_max);
    j_puts(" counts, taken off)\n");
#undef m
}

/* Role 3: call as fast as possible until TSC `to`, counting the round
 * trips that start from `from` on, then report to the root on slot 2: the
 * count; the ruler's counts summed, and how many times it was read (once
 * every 2^16 calls, from `from` on: the CPU's clock over the window);
 * and whether this thread started only after `from`. */
static __attribute__((noreturn)) void timed(uint64_t from, uint64_t to)
{
    struct j_msg m = {.r = {1, 2, 3, 4}};
    uint64_t late = work_tsc() >= from;
    while (work_tsc() < from) {
        (void)once(&m);
    }
    uint64_t n = 0;
    uint64_t rsum = 0;
    uint64_t rn = 0;
    while (work_tsc() < to) {
        if ((n & 0xffffu) == 0) {
            rsum += ruler();
            rn++;
        }
        m.caps = 0;
        if (j_call(1, &m, J_FOREVER) != J_OK) {
            j_puts("ipcbench: call failed\n");
            j_exit(1);
        }
        n++;
    }
    m.r[0] = n;
    m.r[1] = rsum;
    m.r[2] = rn;
    m.r[3] = late;
    m.caps = 0;
    (void)j_call(2, &m, J_FOREVER); /* tell the root */
    j_exit(0);
}

void _start(uint64_t role, uint64_t name, uint64_t from, uint64_t to)
{
    if (role == 0) {
        serve();
    }
    if (role == 3) {
        timed(from, to);
    }
    if (role == 2) {
        for (;;) { /* keep the kernel busy from another CPU (step 9: the lock) */
            (void)j_cap_delete(J_SLOT_NONE);
        }
    }
    struct j_msg m = {.r = {1, 2, 3, 4}};
    bench_name = name;
    if (name == 4) {
        words = 512;
        if (j_ipc_buffer(buf) != J_OK) {
            j_puts("ipcbench: ipc_buffer refused\n");
            j_exit(1);
        }
    }
    if (name == 0) {
        nothing = 1;
        measure("a system call that does nothing", &m);
        nothing = 0;
    }
    measure(name < sizeof names / sizeof names[0] ? names[name] : "?", &m);
    m.caps = 0;
    (void)j_call(2, &m, J_FOREVER); /* tell benchroot */
    j_exit(0);
}
