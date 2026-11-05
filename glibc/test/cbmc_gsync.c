/* SPDX-License-Identifier: GPL-3.0-only */
/* cbmc_gsync.c - CBMC proofs for ../sysdeps/joist/joist-gsync.c (cbmc.sh).
 *
 * From any well-formed table (2 buckets; waiters 1-3 queued or not, in any
 * order, each on one of 3 addresses), waiter 0 calls __jg_wait. Where it
 * would block, another thread does any one wake or requeue (or nothing),
 * and the wait returns signalled or timed out; a second block returns
 * timed out. Then, and after a wake or requeue alone (HARNESS_OP), checked:
 * the table is well formed; waiter 0 is off every queue; every signal went
 * to a waiter already off its queue; a wake's count is the signals it
 * sent; the wait returned JG_CHANGED just when its word wasn't the value
 * expected, and JG_WOKEN just when waiter 0 was signalled (once at most);
 * and after a wake or requeue alone, that it woke just the waiters it
 * should (all on its address, or the first in queue order, or none), that
 * a requeue's other waiters on its source now wait on its destination,
 * and that every other waiter is as it was; a wake told to, stores its
 * value first; a wait with a timeout never blocks without one.
 * Memory safety and unwinding by CBMC's own checks.
 *
 * One run per kind of step (HARNESS_STEP: 0 a wake, 1 a requeue, 2 none),
 * a requeue's source and destination fixed per run (HARNESS_SRC,
 * HARNESS_DST), waiter 0 on word 0: with all three nondet, one run was
 * past CBMC's reach (out of memory at 8 GB). cbmc.sh runs every source
 * and destination.
 *
 * The test hash puts word 1 in bucket 1 and words 0 and 2 in bucket 0, so
 * the runs have one address, two in one bucket, and two buckets. (A hash
 * of the address's bits put all three in bucket 0: CBMC aligns every
 * object to 8 bytes, so no run ever had two buckets.) */
#include <stdint.h>
static uint32_t word0, word1, word2;
#define JG_BUCKETS 2u
#define JG_TEST_HASH(a) ((a) == (uintptr_t)&word1 ? 1u : 0u)
#include "../sysdeps/joist/joist-gsync.c"

#if !defined HARNESS_STEP || !defined HARNESS_SRC || !defined HARNESS_DST
#error "HARNESS_STEP, HARNESS_SRC and HARNESS_DST, as cbmc.sh gives them"
#endif

#define NW 4u
static struct jg_table t;
/* Each waiter and each word (above) its own object: through a pointer into an
 * array of them every access is a byte extract at a symbolic offset, and
 * the formula ran out of memory. */
static struct jg_waiter w0, w1, w2, w3;
static struct jg_waiter *const w[NW] = {&w0, &w1, &w2, &w3};
static unsigned signals;     /* to waiters' notifications */
static unsigned signals0;    /* to waiter 0's */
static unsigned woken;       /* bit i: waiter i was signalled */
static const void *wake_addr; /* the other thread's step's choices */
static int wake_all, requeue_first;
static int wake_mutate;      /* op mode only: in a wait, no step writes a word */
static uint32_t wake_val;
static uint64_t wait_timeout; /* waiter 0's */
static unsigned seq[NW];     /* the order build() queued waiters in, from 1 */
static unsigned blocks;      /* waits made by waiter 0 */
static int in_other;         /* the other thread's step is running */

unsigned nondet_unsigned(void);
int nondet_int(void);
uint64_t nondet_u64(void);

static const void *addr_of(unsigned i)
{
    i %= 3u;
    return i == 0u ? &word0 : i == 1u ? &word1 : &word2;
}

void __joist_gsync_signal(uint32_t n)
{
    if (n < NW) { /* a waiter's (notifications NW.. are the buckets') */
        __CPROVER_assert(w[n]->queued == 0, "a signalled waiter is off its queue");
        signals++;
        signals0 += n == 0u;
        woken |= 1u << n;
    }
}

uint64_t __joist_gsync_now(void)
{
    return nondet_u64();
}

/* The other thread's step: any wake or requeue, or none. */
static unsigned other_step(void)
{
    unsigned n = 0;
    in_other = 1;
    switch (HARNESS_STEP) {
    case 0:
        n = __jg_wake(&t, wake_addr, wake_all, wake_mutate, wake_val, 0);
        break;
    case 1:
        n = __jg_requeue(&t, addr_of(HARNESS_SRC), addr_of(HARNESS_DST), requeue_first);
        break;
    default:
        break;
    }
    in_other = 0;
    return n;
}

int __joist_gsync_wait(uint32_t n, uint64_t timeout)
{
    /* Only waiter 0 blocks here: the buckets' locks are always free in a
     * single thread. */
    __CPROVER_assert(n == 0u && !in_other, "only waiter 0 waits");
    /* Checked; no path goes on from a failure (it would recurse). */
    __CPROVER_assume(n == 0u && !in_other);
    __CPROVER_assert(wait_timeout == JG_FOREVER || timeout != JG_FOREVER, "a wait with a timeout blocks with one");
    if (blocks++ == 0) {
        unsigned before = signals;
        unsigned woke = other_step();
        __CPROVER_assert(signals - before == woke, "a wake's count is its signals");
        return nondet_int() & 1;
    }
    return 1;
}

/* Build any well-formed table: waiters 1..3 in a chosen order, each queued
 * or not, on a chosen address. */
static void build(void)
{
    uint32_t bn[JG_BUCKETS] = {NW, NW + 1u};
    __jg_init(&t, bn);
    for (unsigned i = 0; i < NW; i++) {
        __jg_waiter_init(w[i], i);
    }
    unsigned order[3] = {nondet_unsigned() % 3u, nondet_unsigned() % 3u, nondet_unsigned() % 3u};
    __CPROVER_assume(order[0] != order[1] && order[0] != order[2] && order[1] != order[2]);
    for (unsigned k = 0; k < 3u; k++) {
        struct jg_waiter *x = w[1u + order[k]];
        if (nondet_int() & 1) {
            uintptr_t key = (uintptr_t)addr_of(nondet_unsigned());
            enqueue(&t.b[__jg_bucket_of(key)], x, key);
            seq[x->notif] = k + 1u;
        }
    }
    word0 = nondet_unsigned();
    word1 = nondet_unsigned();
    word2 = nondet_unsigned();
    wake_addr = addr_of(nondet_unsigned());
    wake_all = nondet_int() & 1;
    requeue_first = nondet_int() & 1;
    __CPROVER_assert(__jg_check(&t, NW) == 0, "built well formed");
}

static void off_queues(const struct jg_waiter *x)
{
    for (unsigned b = 0; b < JG_BUCKETS; b++) {
        for (const struct jg_waiter *y = t.b[b].head; y != NULL; y = y->next) {
            __CPROVER_assert(y != x, "off every queue");
        }
    }
}

int main(void)
{
    __CPROVER_assert(__jg_bucket_of((uintptr_t)&word0) == 0u && __jg_bucket_of((uintptr_t)&word2) == 0u &&
                         __jg_bucket_of((uintptr_t)&word1) == 1u,
                     "both buckets in use");
    build();
#ifdef HARNESS_OP
    /* A wake or requeue alone, and the waiters it should take: those on
     * its address (k), the first of them the one queued first. */
    uintptr_t k = (uintptr_t)(HARNESS_STEP == 1 ? addr_of(HARNESS_SRC) : wake_addr);
    uintptr_t dkey = (uintptr_t)addr_of(HARNESS_DST);
    unsigned on = 0, first = NW;
    uintptr_t key0[NW];
    uint8_t queued0[NW];
    for (unsigned i = 1; i < NW; i++) {
        key0[i] = w[i]->key;
        queued0[i] = w[i]->queued;
        on |= (unsigned)(w[i]->queued && w[i]->key == k) << i;
    }
    for (unsigned i = 1; i < NW; i++) {
        if (((on >> i) & 1u) && (first == NW || seq[i] < seq[first])) {
            first = i;
        }
    }
    wake_mutate = nondet_int() & 1;
    wake_val = nondet_unsigned();
    unsigned before = signals;
    unsigned n = other_step();
    __CPROVER_assert(signals - before == n, "a wake's count is its signals");
    if (HARNESS_STEP == 0 && wake_mutate) {
        __CPROVER_assert(*(const uint32_t *)wake_addr == wake_val, "a wake's store made");
    }
    unsigned one = first < NW ? 1u << first : 0u;
    if (HARNESS_STEP == 0) {
        __CPROVER_assert(woken == (wake_all ? on : one), "a wake wakes all, or the first in order");
    } else if (HARNESS_STEP == 1) {
        __CPROVER_assert(woken == (requeue_first ? one : 0u), "a requeue wakes the first in order, or none");
    } else {
        __CPROVER_assert(woken == 0u, "nothing woken");
    }
    for (unsigned i = 1; i < NW; i++) {
        if ((woken >> i) & 1u) {
            continue;
        }
        if (HARNESS_STEP == 1 && ((on >> i) & 1u)) {
            __CPROVER_assert(w[i]->queued && w[i]->key == dkey, "requeued to the destination");
        } else {
            __CPROVER_assert(w[i]->queued == queued0[i] && w[i]->key == key0[i], "any other waiter as it was");
        }
    }
#else
    uint32_t expect = nondet_unsigned();
    int changed = word0 != expect; /* no step writes a word */
    wait_timeout = nondet_u64();
    int r = __jg_wait(&t, w[0], &word0, expect, 0, wait_timeout);
    __CPROVER_assert(r == JG_WOKEN || r == JG_CHANGED || r == JG_TIMEOUT, "a known result");
    __CPROVER_assert(w[0]->queued == 0, "waiter 0 off its queue");
    __CPROVER_assert((r == JG_CHANGED) == changed, "no wait just when the word changed");
    __CPROVER_assert(signals0 <= 1u && (r == JG_WOKEN) == (signals0 == 1u), "woken just when signalled");
    off_queues(w[0]);
#endif
    __CPROVER_assert(__jg_check(&t, NW) == 0, "well formed after");
    return 0;
}
