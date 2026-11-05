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
 * sent. Memory safety and unwinding by CBMC's own checks. */
#define JG_BUCKETS 2u
#define JG_TEST_HASH(a) ((unsigned int)((a) >> 2) & 1u)
#include "../sysdeps/joist/joist-gsync.c"

#define NW 4u
static struct jg_table t;
static struct jg_waiter w[NW];
static uint32_t words[3];
static unsigned signals;     /* to waiters' notifications */
static unsigned blocks;      /* waits made by waiter 0 */
static int in_other;         /* the other thread's step is running */

unsigned nondet_unsigned(void);
int nondet_int(void);
uint64_t nondet_u64(void);

static const void *addr_of(unsigned i)
{
    return &words[i % 3u];
}

void __joist_gsync_signal(uint32_t n)
{
    if (n < NW) { /* a waiter's (notifications NW.. are the buckets') */
        __CPROVER_assert(w[n].queued == 0, "a signalled waiter is off its queue");
        signals++;
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
    switch (nondet_unsigned() % 3u) {
    case 0:
        n = __jg_wake(&t, addr_of(nondet_unsigned()), nondet_int() & 1, 0, 0, 0);
        break;
    case 1:
        n = __jg_requeue(&t, addr_of(nondet_unsigned()), addr_of(nondet_unsigned()), nondet_int() & 1);
        break;
    default:
        break;
    }
    in_other = 0;
    return n;
}

int __joist_gsync_wait(uint32_t n, uint64_t timeout)
{
    (void)timeout;
    /* Only waiter 0 blocks here: the buckets' locks are always free in a
     * single thread. */
    __CPROVER_assert(n == 0u && !in_other, "only waiter 0 waits");
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
        __jg_waiter_init(&w[i], i);
    }
    unsigned order[3] = {nondet_unsigned() % 3u, nondet_unsigned() % 3u, nondet_unsigned() % 3u};
    __CPROVER_assume(order[0] != order[1] && order[0] != order[2] && order[1] != order[2]);
    for (unsigned k = 0; k < 3u; k++) {
        struct jg_waiter *x = &w[1u + order[k]];
        if (nondet_int() & 1) {
            uintptr_t key = (uintptr_t)addr_of(nondet_unsigned());
            enqueue(&t.b[__jg_bucket_of(key)], x, key);
        }
    }
    words[0] = nondet_unsigned();
    words[1] = nondet_unsigned();
    words[2] = nondet_unsigned();
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
    build();
#ifdef HARNESS_OP
    /* A wake or requeue alone. */
    unsigned before = signals;
    unsigned n = other_step();
    __CPROVER_assert(signals - before == n, "a wake's count is its signals");
#else
    int r = __jg_wait(&t, &w[0], addr_of(nondet_unsigned()), nondet_unsigned(), 0, nondet_u64());
    __CPROVER_assert(r == JG_WOKEN || r == JG_CHANGED || r == JG_TIMEOUT, "a known result");
    __CPROVER_assert(w[0].queued == 0, "waiter 0 off its queue");
    off_queues(&w[0]);
#endif
    __CPROVER_assert(__jg_check(&t, NW) == 0, "well formed after");
    return 0;
}
