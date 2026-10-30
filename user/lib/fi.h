/* SPDX-License-Identifier: GPL-3.0-only */
/* fi.h - what the fault-injection test's root task (faultinj) and its peers
 * (fiworker) share (Phase 3's exit): one page per peer task, holding a
 * record per thread. The root writes a command in a thread's record and
 * signals the thread's go notification; the thread carries it out and
 * writes back what the call returned. One command at a time, so the root
 * always knows how far each thread has got: a command started but not done
 * is a call the thread is in, or about to be in. */
#ifndef JOIST_USER_FI_H
#define JOIST_USER_FI_H

#include <stdint.h>

#define FI_VA 0x30000000ull /* a peer's page, in the peer */

/* Slots in a peer task, filled by the root as each scenario needs. */
enum {
    FI_EP = 0,    /* the endpoint: RECV for a server, a badged SEND for a client */
    FI_GO0 = 1,   /* thread 0's go notification (RECV) */
    FI_GO1 = 2,   /* thread 1's */
    FI_REPLY = 3, /* a reply object */
    FI_X = 4,     /* a capability for a client to pass in its Call */
    FI_NS = 5,    /* a badged SEND to the root's notification, to bind no-senders to */
    FI_GOT = 6,   /* where a capability passed to a server lands */
    FI_N = 7,     /* a notification to wait on (RECV) */
    FI_SIG = 8,   /* FI_NSIG badged SENDs to that notification, one bit each */
    FI_NSIG = 16
};

/* Commands. The arguments are a[0..3]; what comes back is res (the call's
 * result) and val[0..2]. */
enum {
    /* Call a[0] with r[0] = a[1], passing a[2] (SEND only) unless it is
     * FI_NOCAP, timeout a[3]; val[0] the reply's r[0]. */
    FI_CALL = 1,
    /* Recv on a[0], timeout a[1]; val[0] the badge, val[1] r[0], val[2]
     * the caps word. */
    FI_RECV,
    /* A passive server's start (docs/abi.md, *Scheduling*): Recv on a[0],
     * then a ReplyRecv answering its starter, which returns as FI_RECV. */
    FI_FIRST,
    FI_REPLY_CMD,   /* Reply with r[0] = a[0] */
    FI_SAVE,        /* reply_save into a[0] */
    FI_REPLY_SAVED, /* reply_saved through a[0] with r[0] = a[1] */
    FI_DELETE,      /* cap_delete a[0] */
    FI_NSNEXT,      /* nosenders_next on a[0]; val[0] the badge */
    FI_BIND_NS,     /* bind_nosenders: endpoint a[0], notification a[1] */
    FI_RECV_SLOTS,  /* capabilities that arrive go to slot a[0] */
    /* Signal FI_SIG + a[0] onwards, a[1] of them in turn, spinning a[2]
     * between: `intent` is how many it has begun, `sent` how many came back. */
    FI_SIGNALS,
    /* Wait on a[0] again and again, OR-ing what comes into `got` (`dups`
     * counts bits that came twice), until a word with FI_STOP comes. */
    FI_WAITS,
    FI_EXIT /* end the thread */
};
#define FI_NOCAP 0xffffu
#define FI_STOP  (1ull << 32)

struct fi_thread {
    volatile uint64_t cmd;
    volatile uint64_t a[4];
    volatile uint64_t posted;  /* commands the root has posted */
    volatile uint64_t started; /* = posted once the thread has begun the last */
    volatile uint64_t done;    /* = posted once it has finished it; res and val are its */
    volatile uint64_t res;
    volatile uint64_t val[3];
    volatile uint64_t intent, sent;     /* FI_SIGNALS */
    volatile uint64_t got, dups, wakes; /* FI_WAITS */
};

struct fi_page {
    struct fi_thread t[2];
};

#endif /* JOIST_USER_FI_H */
