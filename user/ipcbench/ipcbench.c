/* ipcbench.c - the two ends of the IPC benchmark (Phase 3 step 8;
 * benchroot starts them, in one task or in two). What a thread is comes in
 * its start registers: RDI the role (0 the server, 1 the client, 2 a thread
 * that does nothing but make system calls), RSI the name of the measurement.
 *
 * The server answers every call on the endpoint in slot 0 with the same
 * message (Recv once, then ReplyRecv: the server loop).
 *
 * The client makes BENCH_WARMUP calls on slot 1, then BENCH_CALLS more,
 * timing each from before the call to after its return with the time stamp
 * counter (lfence; rdtsc at both ends, so the figure includes one such
 * pair, measured and taken off). The times of a segment go into a
 * histogram, which gives the segment's median and 99th percentile; what is
 * printed is the median over the segments of each, with the worst
 * segment's median beside it, and the least and greatest single time.
 * Then it tells benchroot on slot 2.
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

void _start(uint64_t role, uint64_t name);

static uint32_t hist[BUCKETS + 1u]; /* the last: everything longer */

static const char *const names[] = {
    "two address spaces, passive server",
    "one address space, passive server",
    "two address spaces, server with its own context",
    "two address spaces, passive server, every other CPU making system calls",
};

static __attribute__((noreturn)) void serve(void)
{
    struct j_msg m = {0};
    if (j_recv(0, &m, J_FOREVER) != J_OK) {
        j_puts("ipcbench: recv failed\n");
        j_exit(1);
    }
    for (;;) {
        m.caps = 0;
        if (j_reply_recv(0, &m, J_FOREVER) != J_OK) {
            j_puts("ipcbench: reply_recv failed\n");
            j_exit(1);
        }
    }
}

static int nothing; /* measure a system call that does nothing instead */

static uint64_t once(struct j_msg *m)
{
    if (nothing) {
        uint64_t t0 = work_tsc();
        (void)j_cap_delete(J_SLOT_NONE); /* refused at once: the way in and out */
        return work_tsc() - t0;
    }
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

static void measure(const char *what, struct j_msg *mp)
{
#define m (*mp)
    /* The clock's own cost: the least of many back-to-back readings. */
    uint64_t clock = UINT64_MAX;
    for (unsigned i = 0; i < 1000u; i++) {
        uint64_t t0 = work_tsc();
        uint64_t d = work_tsc() - t0;
        clock = d < clock ? d : clock;
    }
    for (unsigned i = 0; i < BENCH_WARMUP; i++) {
        (void)once(&m);
    }
    /* Buckets of 4 cycles: up to 16384, and one for everything longer. */
    const uint64_t width = 4;
    const uint64_t cycles = 3u * BENCH_RULER;
    uint64_t min = UINT64_MAX;
    uint64_t max = 0;
    uint64_t ruler_min = UINT64_MAX;
    uint64_t ruler_max = 0;
    uint64_t r = 1;
    static uint64_t p50[BENCH_SEGMENTS];
    static uint64_t p99[BENCH_SEGMENTS];
    uint64_t worst = 0;
    for (unsigned seg = 0; seg < BENCH_SEGMENTS; seg++) {
        for (unsigned b = 0; b <= BUCKETS; b++) {
            hist[b] = 0;
        }
        for (unsigned i = 0; i < BENCH_CALLS; i++) {
            if (i % BENCH_BATCH == 0) {
                r = ruler();
                ruler_min = r < ruler_min ? r : ruler_min;
                ruler_max = r > ruler_max ? r : ruler_max;
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
    j_puts("ipcbench: ");
    j_puts(what);
    put(": n=", BENCH_SEGMENTS);
    put("x", BENCH_CALLS);
    put(" min=", min);
    put(" p50=", middle(p50, BENCH_SEGMENTS));
    put(" p99=", middle(p99, BENCH_SEGMENTS));
    put(" worst-p50=", worst);
    put(" max=", max);
    put(" cycles (", cycles);
    put(" cycles took ", ruler_min);
    put(" to ", ruler_max);
    put(" counts; clock ", clock);
    j_puts(" counts, taken off)\n");
#undef m
}

void _start(uint64_t role, uint64_t name)
{
    if (role == 0) {
        serve();
    }
    if (role == 2) {
        for (;;) { /* keep the kernel busy from another CPU (step 9: the lock) */
            (void)j_cap_delete(J_SLOT_NONE);
        }
    }
    struct j_msg m = {.r = {1, 2, 3, 4}};
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
