/* SPDX-License-Identifier: GPL-3.0-only */
/* test_gsync.c - host tests of glibc/sysdeps/joist/joist-gsync.c (0014 B;
 * Phase 5 step 7): the futex queues on emulated notifications
 * (notif_host.c), with real threads.
 *
 * Directed tests (a mismatch, a timeout, FIFO wake-ups, a broadcast, a
 * mutating wake, requeues to another bucket and within one), then stress
 * under seeded randomness: mutexes and a broadcast barrier over many
 * addresses, some of them sharing buckets, with timed waits mixed in. Every
 * test checks the table is well formed and empty after it (__jg_check), and
 * the stress tests that the wake-ups wakers report are exactly those
 * waiters return (none lost, none twice).
 *
 * usage: test-gsync [SEEDS [THREADS]] */
#include "joist-gsync.h"
#include "notif_host.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXT 32u

static struct jg_table T;
static struct jg_waiter W[MAXT];
static unsigned failed;

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "test-gsync: %s:%d: failed: %s\n", __FILE__, __LINE__, #c); \
            failed++;                                                             \
        }                                                                         \
    } while (0)

static void setup(unsigned nthreads)
{
    notif_reset();
    uint64_t bn[JG_BUCKETS];
    for (unsigned i = 0; i < JG_BUCKETS; i++) {
        bn[i] = notif_new();
    }
    __jg_init(&T, bn);
    for (unsigned i = 0; i < nthreads; i++) {
        __jg_waiter_init(&W[i], notif_new());
    }
}

/* The table empty and well formed: every waiter returned. */
static void check_empty(const char *what)
{
    int c = __jg_check(&T, MAXT);
    unsigned left = 0;
    for (unsigned i = 0; i < JG_BUCKETS; i++) {
        left += T.b[i].head != NULL;
    }
    if (c != 0 || left != 0) {
        fprintf(stderr, "test-gsync: %s: table %s (check %d)\n", what, c ? "malformed" : "not empty", c);
        failed++;
    }
}

/* Wait until thread i is blocked in its Wait (so queued). */
static void until_blocked(unsigned i)
{
    for (unsigned n = 0; notif_blocked((uint32_t)W[i].notif) == 0; n++) {
        if (n > 20000u) {
            fprintf(stderr, "test-gsync: thread %u never waited\n", i);
            abort();
        }
        usleep(50);
    }
}

/* Two addresses in one bucket, and a third in another. */
static uint32_t words[4096];
static uint32_t *same_a, *same_b, *other;
static void pick_addresses(void)
{
    same_a = &words[0];
    unsigned ba = __jg_bucket_of((uintptr_t)same_a);
    same_b = other = NULL;
    for (unsigned i = 1; i < 4096u && (!same_b || !other); i++) {
        unsigned b = __jg_bucket_of((uintptr_t)&words[i]);
        if (b == ba && !same_b) {
            same_b = &words[i];
        } else if (b != ba && !other) {
            other = &words[i];
        }
    }
    if (!same_b || !other) {
        abort();
    }
}

struct waitarg {
    unsigned i;
    const void *addr;
    uint64_t expect;
    uint64_t timeout;
    int result;
};

static void *waiter(void *p)
{
    struct waitarg *a = p;
    a->result = __jg_wait(&T, &W[a->i], a->addr, a->expect, 0, a->timeout);
    return NULL;
}

static void directed(void)
{
    pthread_t th[8];
    struct waitarg a[8];

    /* A mismatch: no wait. */
    setup(8);
    words[0] = 5;
    CHECK(__jg_wait(&T, &W[0], &words[0], 4, 0, JG_FOREVER) == JG_CHANGED);
    uint64_t q = 0x100000005ull;
    CHECK(__jg_wait(&T, &W[0], &q, 5, 1, JG_FOREVER) == JG_CHANGED);
    check_empty("mismatch");

    /* A timeout, nobody waking. */
    uint64_t t0 = __joist_gsync_now();
    CHECK(__jg_wait(&T, &W[0], &words[0], 5, 0, 3000000u) == JG_TIMEOUT);
    CHECK(__joist_gsync_now() - t0 >= 3000000u);
    CHECK(__jg_wait(&T, &W[0], &q, 0x100000005ull, 1, 0) == JG_TIMEOUT);
    check_empty("timeout");
    CHECK(__jg_wake(&T, &q, 1, 1, 0x200000009ull, 1) == 0);
    CHECK(q == 0x200000009ull); /* a 64-bit word stored whole */

    /* Wake one at a time: first in, first out. */
    for (unsigned i = 0; i < 4; i++) {
        a[i] = (struct waitarg){i, same_a, *same_a, JG_FOREVER, -1};
        pthread_create(&th[i], NULL, waiter, &a[i]);
        until_blocked(i);
    }
    CHECK(__jg_wake(&T, same_b, 0, 0, 0, 0) == 0); /* the same bucket, another address */
    for (unsigned i = 0; i < 4; i++) {
        CHECK(__jg_wake(&T, same_a, 0, 0, 0, 0) == 1);
        pthread_join(th[i], NULL); /* the first queued is the one woken */
        CHECK(a[i].result == JG_WOKEN);
    }
    CHECK(__jg_wake(&T, same_a, 0, 0, 0, 0) == 0);
    check_empty("fifo");

    /* A broadcast, with a mutation. */
    *same_a = 7;
    for (unsigned i = 0; i < 6; i++) {
        a[i] = (struct waitarg){i, same_a, 7, JG_FOREVER, -1};
        pthread_create(&th[i], NULL, waiter, &a[i]);
        until_blocked(i);
    }
    CHECK(__jg_wake(&T, same_a, 1, 1, 8, 0) == 6);
    CHECK(*same_a == 8);
    for (unsigned i = 0; i < 6; i++) {
        pthread_join(th[i], NULL);
        CHECK(a[i].result == JG_WOKEN);
    }
    check_empty("broadcast");

    /* Requeue to another bucket: one woken, the rest moved. */
    for (int same = 0; same < 2; same++) {
        uint32_t *dst = same ? same_b : other;
        for (unsigned i = 0; i < 5; i++) {
            a[i] = (struct waitarg){i, same_a, *same_a, JG_FOREVER, -1};
            pthread_create(&th[i], NULL, waiter, &a[i]);
            until_blocked(i);
        }
        CHECK(__jg_requeue(&T, same_a, dst, 1, 1) == 1);
        pthread_join(th[0], NULL);
        CHECK(a[0].result == JG_WOKEN);
        CHECK(__jg_check(&T, MAXT) == 0);
        CHECK(__jg_wake(&T, same_a, 1, 0, 0, 0) == 0); /* none left on the source */
        CHECK(__jg_requeue(&T, same_a, dst, 0, 1) == 0);
        CHECK(__jg_wake(&T, dst, 0, 0, 0, 0) == 1);     /* FIFO kept on the target */
        pthread_join(th[1], NULL);
        CHECK(a[1].result == JG_WOKEN);
        CHECK(__jg_wake(&T, dst, 1, 0, 0, 0) == 3);
        for (unsigned i = 2; i < 5; i++) {
            pthread_join(th[i], NULL);
            CHECK(a[i].result == JG_WOKEN);
        }
        check_empty(same ? "requeue, one bucket" : "requeue, two buckets");
    }

    /* Without ALL, one moves: the first; the rest stay. */
    for (unsigned i = 0; i < 3; i++) {
        a[i] = (struct waitarg){i, same_a, *same_a, JG_FOREVER, -1};
        pthread_create(&th[i], NULL, waiter, &a[i]);
        until_blocked(i);
    }
    CHECK(__jg_requeue(&T, same_a, other, 0, 0) == 0);
    CHECK(__jg_wake(&T, other, 1, 0, 0, 0) == 1);
    pthread_join(th[0], NULL);
    CHECK(a[0].result == JG_WOKEN);
    CHECK(__jg_wake(&T, same_a, 1, 0, 0, 0) == 2);
    pthread_join(th[1], NULL);
    pthread_join(th[2], NULL);
    check_empty("requeue one");

    /* A requeued waiter times out from its new bucket. */
    a[0] = (struct waitarg){0, same_a, *same_a, 200000000u, -1};
    pthread_create(&th[0], NULL, waiter, &a[0]);
    until_blocked(0);
    CHECK(__jg_requeue(&T, same_a, other, 0, 1) == 0);
    pthread_join(th[0], NULL);
    CHECK(a[0].result == JG_TIMEOUT);
    check_empty("requeued timeout");
}

/* Stress. */
static unsigned nthreads = 8;
static unsigned long seed;
#define NLOCKS 6u
static uint32_t *lockw[NLOCKS];
static unsigned long counter[NLOCKS], held[NLOCKS];
static unsigned long woken_reported, woken_returned, timeouts;
static uint32_t barrier_seq;
static unsigned barrier_count;
static uint32_t barrier_lock;

static uint64_t rnd(uint64_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static void note(int r)
{
    if (r == JG_WOKEN) {
        __atomic_fetch_add(&woken_returned, 1, __ATOMIC_RELAXED);
    } else if (r == JG_TIMEOUT) {
        __atomic_fetch_add(&timeouts, 1, __ATOMIC_RELAXED);
    }
}

static void wake(const void *addr, int all)
{
    unsigned n = __jg_wake(&T, addr, all, 0, 0, 0);
    __atomic_fetch_add(&woken_reported, n, __ATOMIC_RELAXED);
}

/* Drepper's mutex on the queues: 0 free, 1 held, 2 held and waited for;
 * a timed attempt gives up (and doesn't count) on a timeout. */
static int mutex_lock(unsigned self, uint32_t *m, uint64_t timeout)
{
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(m, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return 1;
    }
    while (__atomic_exchange_n(m, 2, __ATOMIC_ACQUIRE) != 0) {
        int r = __jg_wait(&T, &W[self], m, 2, 0, timeout);
        note(r);
        if (r == JG_TIMEOUT) {
            return 0;
        }
    }
    return 1;
}

static void mutex_unlock(uint32_t *m)
{
    if (__atomic_exchange_n(m, 0, __ATOMIC_RELEASE) == 2) {
        wake(m, 0);
    }
}

/* All threads meet: the last bumps the sequence and wakes everyone. */
static void barrier(unsigned self)
{
    mutex_lock(self, &barrier_lock, JG_FOREVER);
    uint32_t seq = barrier_seq;
    if (++barrier_count == nthreads) {
        barrier_count = 0;
        __atomic_store_n(&barrier_seq, seq + 1, __ATOMIC_RELEASE);
        mutex_unlock(&barrier_lock);
        wake(&barrier_seq, 1);
        return;
    }
    mutex_unlock(&barrier_lock);
    while (__atomic_load_n(&barrier_seq, __ATOMIC_ACQUIRE) == seq) {
        note(__jg_wait(&T, &W[self], &barrier_seq, seq, 0, JG_FOREVER));
    }
}

#define ROUNDS 6u
#define ITERS 300u

static void *worker(void *p)
{
    unsigned self = (unsigned)(uintptr_t)p;
    uint64_t s = seed * 7919u + self + 1u;
    for (unsigned r = 0; r < ROUNDS; r++) {
        for (unsigned i = 0; i < ITERS; i++) {
            unsigned l = (unsigned)(rnd(&s) % NLOCKS);
            uint64_t timeout = rnd(&s) % 4u == 0 ? rnd(&s) % 200000u : JG_FOREVER;
            if (!mutex_lock(self, lockw[l], timeout)) {
                continue;
            }
            if (held[l]++ != 0) {
                fprintf(stderr, "test-gsync: seed %lu: lock %u held twice\n", seed, l);
                abort();
            }
            counter[l]++;
            /* Sometimes held a while: timed waiters then time out, some
             * as they are woken. */
            if (rnd(&s) % 8u == 0) {
                usleep((useconds_t)(rnd(&s) % 150u));
            }
            held[l]--;
            mutex_unlock(lockw[l]);
        }
        barrier(self);
    }
    return NULL;
}

static void stress(unsigned long s)
{
    seed = s;
    setup(nthreads);
    /* Locks in few buckets: two share one with the barrier's word. */
    lockw[0] = same_a;
    lockw[1] = same_b;
    lockw[2] = other;
    for (unsigned i = 3; i < NLOCKS; i++) {
        lockw[i] = &words[100u + i * 37u];
    }
    for (unsigned i = 0; i < NLOCKS; i++) {
        *lockw[i] = 0;
        counter[i] = held[i] = 0;
    }
    woken_reported = woken_returned = timeouts = 0;
    barrier_count = 0;
    barrier_lock = 0;
    pthread_t th[MAXT];
    for (unsigned i = 0; i < nthreads; i++) {
        pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)i);
    }
    for (unsigned i = 0; i < nthreads; i++) {
        pthread_join(th[i], NULL);
    }
    unsigned long total = 0;
    for (unsigned i = 0; i < NLOCKS; i++) {
        total += counter[i];
        if (*lockw[i] != 0) {
            fprintf(stderr, "test-gsync: seed %lu: lock %u left held\n", s, i);
            failed++;
        }
    }
    /* Each wake-up a waker reported is returned by exactly one waiter, which
     * then returns WOKEN; no waiter returns WOKEN without one. */
    if (woken_reported != woken_returned) {
        fprintf(stderr, "test-gsync: seed %lu: %lu woken by wakers, %lu returned\n", s, woken_reported,
                woken_returned);
        failed++;
    }
    if (total == 0) {
        fprintf(stderr, "test-gsync: seed %lu: nothing done\n", s);
        failed++;
    }
    check_empty("stress");
}

/* Requeues racing timeouts: waiters wait, with short timeouts, on X and Y
 * (whose words never change), while a mover requeues them between the two
 * (in different buckets), waking one now and then, and wakes. A waiter
 * timing out as it is moved must find its new bucket. */
static uint32_t *rq_x, *rq_y;
static int rq_done;

static void *rq_waiter(void *p)
{
    unsigned self = (unsigned)(uintptr_t)p;
    uint64_t s = seed * 31u + self + 1u;
    for (unsigned i = 0; i < 1500u; i++) {
        const void *a = rnd(&s) % 2u ? rq_x : rq_y;
        note(__jg_wait(&T, &W[self], a, 0, 0, rnd(&s) % 40000u));
    }
    return NULL;
}

static void *rq_mover(void *p)
{
    uint64_t s = seed * 53u + 7u;
    (void)p;
    while (!__atomic_load_n(&rq_done, __ATOMIC_ACQUIRE)) {
        unsigned n;
        switch (rnd(&s) % 4u) {
        case 0:
            n = __jg_requeue(&T, rq_x, rq_y, rnd(&s) % 4u == 0, (int)(rnd(&s) % 2u));
            break;
        case 1:
            n = __jg_requeue(&T, rq_y, rq_x, rnd(&s) % 4u == 0, (int)(rnd(&s) % 2u));
            break;
        default:
            n = __jg_wake(&T, rnd(&s) % 2u ? rq_x : rq_y, (int)(rnd(&s) % 2u), 0, 0, 0);
            break;
        }
        __atomic_fetch_add(&woken_reported, n, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void requeue_stress(unsigned long s)
{
    seed = s;
    setup(nthreads);
    rq_x = other;
    rq_y = same_a;
    *rq_x = *rq_y = 0;
    woken_reported = woken_returned = timeouts = 0;
    rq_done = 0;
    pthread_t th[MAXT], mv;
    pthread_create(&mv, NULL, rq_mover, NULL);
    for (unsigned i = 0; i < nthreads; i++) {
        pthread_create(&th[i], NULL, rq_waiter, (void *)(uintptr_t)i);
    }
    for (unsigned i = 0; i < nthreads; i++) {
        pthread_join(th[i], NULL);
    }
    __atomic_store_n(&rq_done, 1, __ATOMIC_RELEASE);
    pthread_join(mv, NULL);
    if (woken_reported != woken_returned) {
        fprintf(stderr, "test-gsync: seed %lu: requeue: %lu woken by wakers, %lu returned\n", s,
                woken_reported, woken_returned);
        failed++;
    }
    check_empty("requeue stress");
}

/* A watchdog: a lost wake-up shows as a hang. */
static void *watchdog(void *p)
{
    (void)p;
    const char *e = getenv("GSYNC_WATCHDOG"); /* seconds; the mutations' runs use a short one */
    sleep(e ? (unsigned)strtoul(e, NULL, 0) : 600u);
    fprintf(stderr, "test-gsync: seed %lu: hung (a lost wake-up?)\n", seed);
    abort();
}

int main(int argc, char **argv)
{
    unsigned long seeds = argc > 1 ? strtoul(argv[1], NULL, 0) : 20u;
    if (argc > 2) {
        nthreads = (unsigned)strtoul(argv[2], NULL, 0);
    }
    if (nthreads < 2 || nthreads > MAXT) {
        nthreads = 8;
    }
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);
    pick_addresses();
    directed();
    unsigned long reported = 0, tos = 0;
    for (unsigned long s = 1; s <= seeds && failed == 0; s++) {
        stress(s);
        reported += woken_reported;
        tos += timeouts;
        requeue_stress(s);
        reported += woken_reported;
        tos += timeouts;
    }
    printf("test-gsync: directed, and %lu seeds of %u threads: %lu wake-ups, %lu timeouts; %u failed\n",
           seeds, nthreads, reported, tos, failed);
    return failed != 0;
}
