/* Futex queues on Joist: Mach's gsync_wait, gsync_wake and gsync_requeue
   for glibc's locks (Joist decision 0014, option B; Phase 5 step 7).
   Copyright (C) 2026 Aditya Kale.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

/* Joist's kernel has no futex table (0014: no wait queue keyed by user
   addresses, whose wake-up cost would be a bound on user data).  Here the
   queues are the task's own:

   - each thread has a notification of its own, on which it alone waits
     (struct jg_waiter, in its thread record);
   - a fixed table of buckets, chosen by the lock's address, holds in each
     a FIFO queue of the threads waiting on addresses in it, linked through
     their records, and a lock guarding it;
   - a waiter re-checks the word under the bucket's lock before it queues
     itself, and a waker takes waiters off the queue and signals each under
     that lock, so no wake-up falls between the check and the wait; a
     signal that comes before the wait is kept by the notification's
     sticky bit.

   Nothing is allocated after a thread is made, and nothing fills.  The
   bucket's lock is itself a lock word and a notification: a waiter for it
   blocks, it never spins (a spinning thread of higher priority would keep
   the holder from running).  */

#ifndef _JOIST_GSYNC_H
#define _JOIST_GSYNC_H 1

#include <stdint.h>

#ifndef JG_BUCKETS
# define JG_BUCKETS 32u
#endif

/* What the library needs from its environment (sysdeps/joist: the
   kernel's calls; the host tests: an emulation of them).  NOTIF is a
   capability slot naming a notification the caller may signal and wait
   on.  */
/* Signal: never blocks.  */
extern void __joist_gsync_signal (uint32_t notif);
/* Wait: 0 when signalled (or a bit was pending), 1 when TIMEOUT
   nanoseconds passed first; TIMEOUT JG_FOREVER waits for good.  */
extern int __joist_gsync_wait (uint32_t notif, uint64_t timeout);
/* The monotonic time, in nanoseconds.  */
extern uint64_t __joist_gsync_now (void);

#define JG_FOREVER UINT64_MAX

/* A thread's place in a queue: in its thread record, for its life.  */
struct jg_waiter
{
  struct jg_waiter *next;	/* in its bucket's queue */
  uintptr_t key;		/* the address it waits on */
  uint32_t notif;		/* its own notification */
  uint32_t queued;		/* 1 while in a queue (atomic) */
};

struct jg_bucket
{
  uint32_t lock;		/* 0 free, 1 held, 2 held and waited for */
  uint32_t notif;		/* where the lock's waiters wait */
  struct jg_waiter *head, *tail;
};

struct jg_table
{
  struct jg_bucket b[JG_BUCKETS];
};

/* T empty, its buckets' locks on the notifications NOTIFS.  */
extern void __jg_init (struct jg_table *t, const uint32_t notifs[JG_BUCKETS]);
/* SELF's notification: before its first wait.  */
extern void __jg_waiter_init (struct jg_waiter *self, uint32_t notif);

enum
{
  JG_WOKEN,			/* a wake took it off the queue */
  JG_CHANGED,			/* the word wasn't EXPECT: no wait */
  JG_TIMEOUT			/* TIMEOUT nanoseconds, no wake */
};

/* Wait on ADDR while it holds EXPECT (a 32-bit word, or 64 if QUAD), for
   at most TIMEOUT nanoseconds (JG_FOREVER: no limit).  */
extern int __jg_wait (struct jg_table *t, struct jg_waiter *self,
		    const void *addr, uint64_t expect, int quad,
		    uint64_t timeout);
/* Wake one waiter on ADDR, or all of them if ALL; first, if MUTATE, store
   VAL there (32 bits, or 64 if QUAD) under the bucket's lock.  The number
   woken.  */
extern unsigned int __jg_wake (struct jg_table *t, const void *addr, int all,
			     int mutate, uint64_t val, int quad);
/* Wake one waiter on SRC if WAKE_ONE, and move the others to wait on DST.
   The number woken (0 or 1).  */
extern unsigned int __jg_requeue (struct jg_table *t, const void *src,
				const void *dst, int wake_one);

/* For the tests: 0 if T is well formed (every queue a list ending at its
   tail, each waiter in it queued and keyed to that bucket, at most MAX
   waiters in all), else a number saying what is wrong.  */
extern int __jg_check (const struct jg_table *t, unsigned int max);
/* The bucket of ADDR.  */
extern unsigned int __jg_bucket_of (uintptr_t addr);

#endif
