/* SPDX-License-Identifier: GPL-3.0-only */
/* notif_host.c - Joist's notifications on the host, for the tests of
 * glibc/sysdeps/joist (docs/abi.md, *Notifications*): a word of pending
 * bits and a queue of waiters. Signal hands the signal to a waiter not
 * yet given one, or sets the bit if there is none; Wait returns at once
 * (clearing the bit) if it is set, otherwise blocks until handed a signal
 * or out of time. A handed signal is never lost to a timeout: the hand-off
 * and the timeout are decided under one lock, as in the kernel. */
#include "notif_host.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

struct notif {
    pthread_mutex_t m;
    pthread_cond_t c;
    int bit;
    unsigned waiting;  /* threads blocked in Wait */
    unsigned handed;   /* signals handed to them, not yet taken */
};

static struct notif notifs[NOTIF_MAX];
static unsigned nnotifs;
static pthread_mutex_t alloc = PTHREAD_MUTEX_INITIALIZER;
unsigned long notif_signals, notif_waits, notif_timeouts;

uint32_t notif_new(void)
{
    pthread_mutex_lock(&alloc);
    if (nnotifs == NOTIF_MAX) {
        abort();
    }
    uint32_t n = nnotifs++;
    pthread_mutex_unlock(&alloc);
    struct notif *x = &notifs[n];
    pthread_mutex_init(&x->m, NULL);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&x->c, &a);
    pthread_condattr_destroy(&a);
    x->bit = 0;
    x->waiting = x->handed = 0;
    return n;
}

void notif_reset(void)
{
    pthread_mutex_lock(&alloc);
    for (unsigned i = 0; i < nnotifs; i++) {
        pthread_mutex_destroy(&notifs[i].m);
        pthread_cond_destroy(&notifs[i].c);
    }
    nnotifs = 0;
    pthread_mutex_unlock(&alloc);
}

unsigned notif_blocked(uint32_t n)
{
    struct notif *x = &notifs[n];
    pthread_mutex_lock(&x->m);
    unsigned w = x->waiting - x->handed;
    pthread_mutex_unlock(&x->m);
    return w;
}

int notif_pending(uint32_t n)
{
    struct notif *x = &notifs[n];
    pthread_mutex_lock(&x->m);
    int b = x->bit;
    pthread_mutex_unlock(&x->m);
    return b;
}

void __joist_gsync_signal(uint64_t n)
{
    struct notif *x = &notifs[n];
    pthread_mutex_lock(&x->m);
    __atomic_fetch_add(&notif_signals, 1, __ATOMIC_RELAXED);
    if (x->waiting > x->handed) {
        x->handed++;
        pthread_cond_broadcast(&x->c);
    } else {
        x->bit = 1;
    }
    pthread_mutex_unlock(&x->m);
}

uint64_t __joist_gsync_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int __joist_gsync_wait(uint64_t n, uint64_t timeout)
{
    struct notif *x = &notifs[n];
    struct timespec dl;
    if (timeout != JG_FOREVER) {
        uint64_t d = __joist_gsync_now() + timeout;
        dl.tv_sec = (time_t)(d / 1000000000ull);
        dl.tv_nsec = (long)(d % 1000000000ull);
    }
    pthread_mutex_lock(&x->m);
    __atomic_fetch_add(&notif_waits, 1, __ATOMIC_RELAXED);
    if (x->bit) {
        x->bit = 0;
        pthread_mutex_unlock(&x->m);
        return 0;
    }
    if (timeout == 0) {
        pthread_mutex_unlock(&x->m);
        __atomic_fetch_add(&notif_timeouts, 1, __ATOMIC_RELAXED);
        return 1;
    }
    x->waiting++;
    int r = 0;
    while (x->handed == 0) {
        r = timeout == JG_FOREVER ? pthread_cond_wait(&x->c, &x->m)
                                  : pthread_cond_timedwait(&x->c, &x->m, &dl);
        if (r == ETIMEDOUT && x->handed == 0) {
            break;
        }
    }
    x->waiting--;
    if (x->handed > 0) {
        x->handed--;
        pthread_mutex_unlock(&x->m);
        return 0;
    }
    pthread_mutex_unlock(&x->m);
    __atomic_fetch_add(&notif_timeouts, 1, __ATOMIC_RELAXED);
    return 1;
}
