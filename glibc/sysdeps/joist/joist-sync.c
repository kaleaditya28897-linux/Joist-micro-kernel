/* Threads' notifications and the futex queues on them: Mach's gsync calls
   on Joist (Phase 5 step 7, the design's items 5 and 7).
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

/* One in a program, as the name table (joist-ports.h): ld.so's if it is
   dynamic, libc.a's if static.  The futex queues (joist-gsync.c) are
   included here, their environment given below: a notification handle is
   the slot of a capability to signal it in its high half, the slot of one
   to wait on in its low.  */

#include "joist-ports.h"
#include "joist-sys.h"
#include "joist-thread.h"
#include <tls.h>
#include <mach.h>
#include <lowlevellock.h>
#include <libc-lock.h>

#if !(defined SHARED && IS_IN (libc))

/* 16 buckets: 32 names and 16 objects of the task's, made once.  */
#define JG_BUCKETS 16u
#include "joist-gsync.c"

#define JOIST_SYS_SIGNAL	14
#define JOIST_SYS_WAIT		15
#define JOIST_SYS_CLOCK		56
#define JOIST_OBJ_NOTIFICATION	2
#define JOIST_E_TIMEOUT		9
#define JOIST_E_STATE		13
#define JOIST_E_ABORTED		14
#define JOIST_SYS_REPLY_SAVE	17
#define JOIST_OBJ_REPLY		5

struct joist_thread __joist_thread0 = { .self = JOIST_SLOT_THREAD };
rtld_hidden_data_def (__joist_thread0)

static struct jg_table table;
static int table_ready;		/* atomic: 1 once the buckets are made */

/* A pinned name for libc's own capability.  */
static int
pinned (uint32_t *name)
{
  struct jn_table *t = __joist_ports_lock ();
  int r = __jn_pinned (t, name);
  __joist_ports_unlock ();
  return r;
}

static void
unpin (uint32_t name)
{
  if (name == 0)
    return;
  (void) __joist_sys (JOIST_SYS_CAP_DELETE, name, 0, 0, 0, NULL);
  struct jn_table *t = __joist_ports_lock ();
  __jn_unpin (t, name);
  __joist_ports_unlock ();
}

/* A notification in a new name, and NBITS badged capabilities to signal
   it (bit i in SIG[i]): 0, or KERN_RESOURCE_SHORTAGE (nothing kept).  */
static int
make_notification (uint32_t *recv, uint32_t sig[], unsigned int nbits)
{
  uint32_t r = 0, s[2] = { 0, 0 };
  int ok = pinned (&r) == JN_OK
	   && __joist_sys (JOIST_SYS_OBJ_CREATE, JOIST_OBJ_NOTIFICATION, r, 0,
			   0, NULL) == JOIST_OK;
  for (unsigned int i = 0; ok && i < nbits; i++)
    ok = pinned (&s[i]) == JN_OK
	 && __joist_sys (JOIST_SYS_CAP_MINT, r, s[i], JOIST_R_SEND,
			 1ull << i, NULL) == JOIST_OK;
  if (!ok)
    {
      for (unsigned int i = 0; i < nbits; i++)
	unpin (s[i]);
      unpin (r);
      return KERN_RESOURCE_SHORTAGE;
    }
  *recv = r;
  for (unsigned int i = 0; i < nbits; i++)
    sig[i] = s[i];
  return 0;
}

int
__joist_thread_make (struct joist_thread *t, uint32_t self)
{
  uint32_t recv, sig[2];
  int r = make_notification (&recv, sig, 2);
  if (r != 0)
    return r;
  t->self = self;
  t->recv = recv;
  t->sig_gsync = sig[0];
  t->sig_htl = sig[1];
  t->kept = 0;
  __jg_waiter_init (&t->waiter, (uint64_t) sig[0] << 32 | recv);
  return 0;
}
rtld_hidden_def (__joist_thread_make)

/* Its names under one lock: a wait for the lock needs T whole when T is
   the calling thread's own (pt-thread-terminate.c).  Its reply object
   too: a reply still saved there wakes its caller (E_PEER) as it goes.  */
void
__joist_thread_free (struct joist_thread *t)
{
  uint32_t names[4] = { t->sig_htl, t->sig_gsync, t->recv, t->reply_obj };
  struct jn_table *tab = __joist_ports_lock ();
  for (unsigned int i = 0; i < 4; i++)
    if (names[i] != 0)
      {
	(void) __joist_sys (JOIST_SYS_CAP_DELETE, names[i], 0, 0, 0, NULL);
	__jn_unpin (tab, names[i]);
      }
  __joist_ports_unlock ();
  t->recv = t->sig_gsync = t->sig_htl = 0;
  t->reply_obj = t->reply_saved = 0;
}
rtld_hidden_def (__joist_thread_free)

static uint64_t
now (void)
{
  uint64_t ns = 0;
  (void) __joist_sys (JOIST_SYS_CLOCK, 0, 0, 0, 0, &ns);
  return ns;
}

int
__joist_reply_obj_make (struct joist_thread *t)
{
  if (t->reply_obj != 0)
    return 0;
  uint32_t r = 0;
  if (pinned (&r) != JN_OK)
    return -1;
  if (__joist_sys (JOIST_SYS_OBJ_CREATE, JOIST_OBJ_REPLY, r, 0, 0, NULL)
      != JOIST_OK)
    {
      unpin (r);
      return -1;
    }
  t->reply_obj = r;
  return 0;
}
rtld_hidden_def (__joist_reply_obj_make)

/* The kernel refused the calling thread T's Wait (JOIST_E_STATE) because
   T holds the reply capability of a call it serves (I10: a Wait with no
   end would strand the caller).  A server's thread waits for locks,
   conditions and sleeps as it serves, as on Mach, so the capability moves
   into T's reply object, and the call's answer goes through it
   (joist-mig.c's server_ipc): 0, and the wait is tried again; or -1 (none
   held, or no object), and the refusal stands.

   The object is made before (__joist_reply_obj_make, at the thread's
   plain receive), never here: this runs inside a wait, maybe for the
   names lock, which making it takes; a second wait for a lock inside the
   first, on the thread's one futex queue entry, loses the first's
   wake-up (the thread then waits for ever: seen as a hang of the pool's
   thread in pthread_create, 2 CPUs).  No lock and no wait here.  */
static int
reply_save (struct joist_thread *t)
{
  if (t->reply_saved || t->reply_obj == 0)
    return -1;			/* refused for another reason, or no object */
  if (__joist_sys (JOIST_SYS_REPLY_SAVE, t->reply_obj, 0, 0, 0, NULL)
      != JOIST_OK)
    return -1;
  t->reply_saved = 1;
  return 0;
}

int
__joist_thread_wait (struct joist_thread *t, uint64_t bit, uint64_t timeout)
{
  if (t->kept & bit)
    {
      t->kept &= ~bit;
      return 0;
    }
  uint64_t deadline = JG_FOREVER;
  if (timeout != JG_FOREVER)
    {
      uint64_t n = now ();
      deadline = timeout < JG_FOREVER - n ? n + timeout : JG_FOREVER - 1;
    }
  for (;;)
    {
      uint64_t left = JG_FOREVER;
      if (deadline != JG_FOREVER)
	{
	  uint64_t n = now ();
	  if (n >= deadline)
	    return 1;
	  left = deadline - n;
	}
      uint64_t bits = 0;
      uint64_t e = __joist_sys (JOIST_SYS_WAIT, t->recv, left, 0, 0, &bits);
      if (e == JOIST_E_STATE && reply_save (t) == 0)
	continue;		/* serving a call: its reply saved, again */
      if (e == JOIST_E_ABORTED)
	return 2;		/* thread_abort or thread_suspend */
      if (e != JOIST_OK)
	return 1;		/* E_TIMEOUT; anything else ends it too */
      t->kept |= bits & ~bit;
      if (bits & bit)
	return 0;
    }
}
rtld_hidden_def (__joist_thread_wait)

void
__joist_thread_signal (const struct joist_thread *t, uint64_t bit)
{
  uint32_t s = bit == JOIST_BIT_HTL ? t->sig_htl : t->sig_gsync;
  (void) __joist_sys (JOIST_SYS_SIGNAL, s, 0, 0, 0, NULL);
}
rtld_hidden_def (__joist_thread_signal)

/* The futex queues' environment (joist-gsync.h).  */

void
__joist_gsync_signal (uint64_t notif)
{
  (void) __joist_sys (JOIST_SYS_SIGNAL, notif >> 32, 0, 0, 0, NULL);
}

int
__joist_gsync_wait (uint64_t notif, uint64_t timeout)
{
  uint32_t recv = (uint32_t) notif;
  struct joist_thread *self = __joist_thread_self ();
  if (recv == self->recv)
    /* Its own: the futex queues' bit, htl's kept for it.  */
    return __joist_thread_wait (self, JOIST_BIT_GSYNC, timeout);
  /* A bucket's lock: its one bit (a server's reply saved first, as
     above).  */
  uint64_t bits;
  uint64_t e = __joist_sys (JOIST_SYS_WAIT, recv, timeout, 0, 0, &bits);
  if (e == JOIST_E_STATE && reply_save (self) == 0)
    e = __joist_sys (JOIST_SYS_WAIT, recv, timeout, 0, 0, &bits);
  return e != JOIST_OK;
}

uint64_t
__joist_gsync_now (void)
{
  return now ();
}

/* Scheduling contexts terminated threads left (pt-thread-terminate.c):
   a thread can't delete its own and go on, so it leaves it here, and the
   next thread started takes it (pt-thread-start.c).  At most KEEP_SC; any
   more are deleted (by their last thread, once it is gone: never, so
   the list is the bound of what is left behind).  */
#define KEEP_SC 16u
static uint32_t kept_sc[KEEP_SC];
__libc_lock_define_initialized (static, sc_lock)

uint32_t
__joist_sc_reuse (void)
{
  uint32_t sc = 0;
  __libc_lock_lock (sc_lock);
  for (unsigned int i = 0; i < KEEP_SC && sc == 0; i++)
    if (kept_sc[i] != 0)
      {
	sc = kept_sc[i];
	kept_sc[i] = 0;
      }
  __libc_lock_unlock (sc_lock);
  return sc;
}
rtld_hidden_def (__joist_sc_reuse)

void
__joist_sc_keep (uint32_t sc)
{
  __libc_lock_lock (sc_lock);
  for (unsigned int i = 0; i < KEEP_SC; i++)
    if (kept_sc[i] == 0)
      {
	kept_sc[i] = sc;
	sc = 0;
	break;
      }
  __libc_lock_unlock (sc_lock);
  if (sc != 0)
    unpin (sc);		/* the list is full: given up, unbound or not */
}
rtld_hidden_def (__joist_sc_keep)

int
__joist_sync_init (void)
{
  if (atomic_load_acquire (&table_ready))
    return 0;
  /* Before the second thread: nothing races here (joist-thread.h).  */
  uint64_t notifs[JG_BUCKETS];
  for (unsigned int i = 0; i < JG_BUCKETS; i++)
    {
      uint32_t recv, sig;
      if (make_notification (&recv, &sig, 1) != 0)
	return KERN_RESOURCE_SHORTAGE;	/* those made stay, for a retry */
      notifs[i] = (uint64_t) sig << 32 | recv;
    }
  __jg_init (&table, notifs);
  atomic_store_release (&table_ready, 1);
  return 0;
}
rtld_hidden_def (__joist_sync_init)

/* The gsync calls (gnumach.defs), on this task's own addresses.  Before
   the table is made the program has one thread: no lock is contended, so
   a wait returns at once and a wake has no one to wake.  */

#define GSYNC_SHARED	0x01
#define GSYNC_QUAD	0x02
#define GSYNC_TIMED	0x04
#define GSYNC_BROADCAST	0x08
#define GSYNC_MUTATE	0x10

kern_return_t
__joist_gsync_wait_call (mach_port_t task, vm_offset_t addr, unsigned int val1,
			 unsigned int val2, natural_t msec, int flags)
{
  if (task != __mach_task_self () || (flags & GSYNC_SHARED))
    return KERN_INVALID_ARGUMENT;	/* another task's word: Phase 7 */
  if (!atomic_load_acquire (&table_ready))
    return KERN_SUCCESS;
  struct joist_thread *self = __joist_thread_self ();
  if (self->recv == 0 && __joist_thread_make (self, self->self) != 0)
    return KERN_RESOURCE_SHORTAGE;
  int quad = (flags & GSYNC_QUAD) != 0;
  uint64_t expect = quad ? (uint64_t) val2 << 32 | val1 : val1;
  uint64_t timeout = (flags & GSYNC_TIMED)
		     ? (uint64_t) msec * 1000000u : JG_FOREVER;
  return __jg_wait (&table, &self->waiter, (const void *) addr, expect, quad,
		    timeout) == JG_TIMEOUT ? KERN_TIMEDOUT : KERN_SUCCESS;
}
rtld_hidden_def (__joist_gsync_wait_call)

kern_return_t
__joist_gsync_wake_call (mach_port_t task, vm_offset_t addr, unsigned int val,
			 int flags)
{
  if (task != __mach_task_self () || (flags & GSYNC_SHARED))
    return KERN_INVALID_ARGUMENT;
  if (!atomic_load_acquire (&table_ready))
    {
      if (flags & GSYNC_MUTATE)
	atomic_store_release ((unsigned int *) addr, val);
      return KERN_SUCCESS;
    }
  (void) __jg_wake (&table, (const void *) addr, (flags & GSYNC_BROADCAST) != 0,
		    (flags & GSYNC_MUTATE) != 0, val, 0);
  return KERN_SUCCESS;
}
rtld_hidden_def (__joist_gsync_wake_call)

kern_return_t
__joist_gsync_requeue_call (mach_port_t task, vm_offset_t src,
			    vm_offset_t dst, boolean_t wake_one, int flags)
{
  if (task != __mach_task_self () || (flags & GSYNC_SHARED))
    return KERN_INVALID_ARGUMENT;
  if (atomic_load_acquire (&table_ready))
    (void) __jg_requeue (&table, (const void *) src, (const void *) dst,
			 wake_one, (flags & GSYNC_BROADCAST) != 0);
  return KERN_SUCCESS;
}
rtld_hidden_def (__joist_gsync_requeue_call)

#endif
