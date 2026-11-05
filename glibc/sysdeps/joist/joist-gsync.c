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

#include "joist-gsync.h"

#include <stddef.h>

/* Atomics: C11's, through the builtins every compiler of glibc has.  */
#define load_relaxed(p) __atomic_load_n ((p), __ATOMIC_RELAXED)
#define load_acquire(p) __atomic_load_n ((p), __ATOMIC_ACQUIRE)
#define store_relaxed(p, v) __atomic_store_n ((p), (v), __ATOMIC_RELAXED)
#define store_release(p, v) __atomic_store_n ((p), (v), __ATOMIC_RELEASE)

unsigned int
__jg_bucket_of (uintptr_t addr)
{
#ifdef JG_TEST_HASH
  /* The CBMC proofs' (test/cbmc_gsync.c): cheap, which bucket being
     beside the point there.  */
  return JG_TEST_HASH (addr);
#endif
  /* Words are 4-byte aligned; a multiplicative hash of the rest.  */
  uint64_t h = (uint64_t) (addr >> 2) * 0x9e3779b97f4a7c15ull;
  return (unsigned int) (h >> 32) % JG_BUCKETS;
}

void
__jg_init (struct jg_table *t, const uint32_t notifs[JG_BUCKETS])
{
  for (unsigned int i = 0; i < JG_BUCKETS; i++)
    {
      t->b[i].lock = 0;
      t->b[i].notif = notifs[i];
      t->b[i].head = t->b[i].tail = NULL;
    }
}

void
__jg_waiter_init (struct jg_waiter *self, uint32_t notif)
{
  self->next = NULL;
  self->key = 0;
  self->notif = notif;
  self->queued = 0;
}

/* The bucket's lock: a word and a notification (a mutex as in Drepper's
   "Futexes Are Tricky", its futex a notification).  A waiter blocks; the
   notification's sticky bit keeps an unlock that comes before the wait.  */
static void
bucket_lock (struct jg_bucket *b)
{
  uint32_t free = 0;
  if (__atomic_compare_exchange_n (&b->lock, &free, 1, 0, __ATOMIC_ACQUIRE,
				   __ATOMIC_RELAXED))
    return;
  while (__atomic_exchange_n (&b->lock, 2, __ATOMIC_ACQUIRE) != 0)
    __joist_gsync_wait (b->notif, JG_FOREVER);
}

static void
bucket_unlock (struct jg_bucket *b)
{
  if (__atomic_exchange_n (&b->lock, 0, __ATOMIC_RELEASE) == 2)
    __joist_gsync_signal (b->notif);
}

/* The word at ADDR (32 or 64 bits).  */
static uint64_t
word (const void *addr, int quad)
{
  if (quad)
    return load_relaxed ((const uint64_t *) addr);
  return load_relaxed ((const uint32_t *) addr);
}

/* With B locked: SELF at the end of B's queue, waiting on KEY.  */
static void
enqueue (struct jg_bucket *b, struct jg_waiter *self, uintptr_t key)
{
  self->next = NULL;
  store_relaxed (&self->key, key);
  if (b->tail != NULL)
    b->tail->next = self;
  else
    b->head = self;
  b->tail = self;
  store_relaxed (&self->queued, 1);
}

/* With B locked: W, which is in B's queue after PREV (NULL: first), out of
   it.  Nothing about W is touched after its QUEUED goes to 0: its thread
   may then return and use its record again.  */
static void
unlink_after (struct jg_bucket *b, struct jg_waiter *prev,
	      struct jg_waiter *w)
{
  if (prev != NULL)
    prev->next = w->next;
  else
    b->head = w->next;
  if (b->tail == w)
    b->tail = prev;
  w->next = NULL;
}

/* With B locked: take W (after PREV) off the queue and signal it.  */
static void
wake_one (struct jg_bucket *b, struct jg_waiter *prev, struct jg_waiter *w)
{
  uint32_t notif = w->notif;
  unlink_after (b, prev, w);
  store_release (&w->queued, 0);
  __joist_gsync_signal (notif);
}

/* Lock the bucket SELF's key is in now (a requeue may move it, under both
   buckets' locks): the bucket, locked, with SELF's key unchanged since.  */
static struct jg_bucket *
lock_own_bucket (struct jg_table *t, struct jg_waiter *self)
{
  for (;;)
    {
      uintptr_t key = load_relaxed (&self->key);
      struct jg_bucket *b = &t->b[__jg_bucket_of (key)];
      bucket_lock (b);
      if (load_relaxed (&self->key) == key)
	return b;
      bucket_unlock (b);
    }
}

int
__jg_wait (struct jg_table *t, struct jg_waiter *self, const void *addr,
	 uint64_t expect, int quad, uint64_t timeout)
{
  uintptr_t key = (uintptr_t) addr;
  struct jg_bucket *b = &t->b[__jg_bucket_of (key)];
  uint64_t deadline = JG_FOREVER;

  if (timeout != JG_FOREVER)
    {
      uint64_t now = __joist_gsync_now ();
      deadline = timeout < JG_FOREVER - now ? now + timeout : JG_FOREVER - 1;
    }

  bucket_lock (b);
  if (word (addr, quad) != expect)
    {
      bucket_unlock (b);
      return JG_CHANGED;
    }
  enqueue (b, self, key);
  bucket_unlock (b);

  for (;;)
    {
      uint64_t left = JG_FOREVER;
      if (deadline != JG_FOREVER)
	{
	  uint64_t now = __joist_gsync_now ();
	  left = now < deadline ? deadline - now : 0;
	}
      int timed_out = left == 0 || __joist_gsync_wait (self->notif, left);
      if (load_acquire (&self->queued) == 0)
	return JG_WOKEN;
      if (!timed_out)
	/* A bit left by an earlier wait's wake-up (it timed out as it was
	   woken): still queued, so wait again.  */
	continue;
      /* Out of time: off the queue, unless a wake took it off first (its
	 signal then stays pending, and makes the next wait's first return
	 a spurious one, which the loop above absorbs).  */
      b = lock_own_bucket (t, self);
      if (load_relaxed (&self->queued) == 0)
	{
	  bucket_unlock (b);
	  return JG_WOKEN;
	}
      struct jg_waiter *prev = NULL;
      for (struct jg_waiter *w = b->head; w != self; w = w->next)
	prev = w;
      unlink_after (b, prev, self);
      store_relaxed (&self->queued, 0);
      bucket_unlock (b);
      return JG_TIMEOUT;
    }
}

unsigned int
__jg_wake (struct jg_table *t, const void *addr, int all, int mutate,
	 uint64_t val, int quad)
{
  uintptr_t key = (uintptr_t) addr;
  struct jg_bucket *b = &t->b[__jg_bucket_of (key)];
  unsigned int n = 0;

  bucket_lock (b);
  if (mutate)
    {
      if (quad)
	__atomic_store_n ((uint64_t *) addr, val, __ATOMIC_RELEASE);
      else
	__atomic_store_n ((uint32_t *) addr, (uint32_t) val, __ATOMIC_RELEASE);
    }
  struct jg_waiter *prev = NULL, *w = b->head;
  while (w != NULL)
    {
      struct jg_waiter *next = w->next;
      if (w->key == key)
	{
	  wake_one (b, prev, w);
	  n++;
	  if (!all)
	    break;
	}
      else
	prev = w;
      w = next;
    }
  bucket_unlock (b);
  return n;
}

unsigned int
__jg_requeue (struct jg_table *t, const void *src, const void *dst,
	    int wake_one_first)
{
  uintptr_t skey = (uintptr_t) src, dkey = (uintptr_t) dst;
  unsigned int si = __jg_bucket_of (skey), di = __jg_bucket_of (dkey);
  struct jg_bucket *sb = &t->b[si], *db = &t->b[di];
  unsigned int n = 0;

  /* Both locks, in bucket order (one if they are the same).  */
  bucket_lock (si <= di ? sb : db);
  if (si != di)
    bucket_lock (si <= di ? db : sb);

  struct jg_waiter *prev = NULL, *w = sb->head;
  while (w != NULL)
    {
      struct jg_waiter *next = w->next;
      if (w->key != skey)
	prev = w;
      else if (wake_one_first && n == 0)
	{
	  wake_one (sb, prev, w);
	  n = 1;
	}
      else if (si == di)
	{
	  /* Same queue: it keeps its place, waiting on DST now.  */
	  store_relaxed (&w->key, dkey);
	  prev = w;
	}
      else
	{
	  unlink_after (sb, prev, w);
	  /* Still queued throughout: its QUEUED stays 1.  */
	  if (db->tail != NULL)
	    db->tail->next = w;
	  else
	    db->head = w;
	  db->tail = w;
	  store_relaxed (&w->key, dkey);
	}
      w = next;
    }

  if (si != di)
    bucket_unlock (si <= di ? db : sb);
  bucket_unlock (si <= di ? sb : db);
  return n;
}

int
__jg_check (const struct jg_table *t, unsigned int max)
{
  unsigned int seen = 0;
  for (unsigned int i = 0; i < JG_BUCKETS; i++)
    {
      const struct jg_bucket *b = &t->b[i];
      const struct jg_waiter *last = NULL;
      if ((b->head == NULL) != (b->tail == NULL))
	return 1;		/* a head without a tail, or a tail alone */
      for (const struct jg_waiter *w = b->head; w != NULL; w = w->next)
	{
	  if (++seen > max)
	    return 2;		/* a cycle, or more waiters than there are */
	  if (w->queued != 1)
	    return 3;		/* in a queue, not marked queued */
	  if (__jg_bucket_of (w->key) != i)
	    return 4;		/* in another address's bucket */
	  last = w;
	}
      if (last != b->tail)
	return 5;		/* the tail isn't the last */
    }
  return 0;
}
