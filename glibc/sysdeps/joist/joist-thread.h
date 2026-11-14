/* A thread's Joist record (Phase 5 step 7, the design's items 5 and 7).
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

/* Each thread has one notification (the design's item 5) on which it alone
   waits.  Two badged capabilities signal it: bit 0 for the futex queues
   (joist-gsync), bit 1 for htl's block and wake-up (the owner's choice,
   2026-11-06: one notification a thread, two badges).  A wait of one kind
   that gets the other's bit keeps it in KEPT, for the next wait of that
   kind: a notification's bits are cleared when a Wait returns them.

   The record is the last member of the TCB (tls.h); the first thread uses
   __joist_thread0 until its TLS is set up, which copies it into its TCB.  */

#ifndef _JOIST_THREAD_H
#define _JOIST_THREAD_H 1

#include <stdint.h>
#include "joist-gsync.h"

/* The slots start-up fixes (the design's item 8): the task's own
   capability, the first thread's, the bootstrap port, the host's name;
   mm's is 8.  */
#define JOIST_SLOT_TASK		1u
#define JOIST_SLOT_THREAD	2u
#define JOIST_SLOT_BOOTSTRAP	3u
/* The host: a name with no capability (mach_host_self.c).  */
#define JOIST_SLOT_HOST		4u

/* A new thread's scheduling context until start-up gives the task its
   parameters (the design's items 7 and 8): a priority below the root's
   servers', and all of its time.  */
#define JOIST_THREAD_PRIO	100u
#define JOIST_THREAD_PERIOD	10000000u	/* ns: 10 ms, all of it */

#define JOIST_BIT_GSYNC		1u
#define JOIST_BIT_HTL		2u
/* A bit no one signals: a sleep's wait (clock_nanosleep.c), which only
   its time running out or an abort ends.  */
#define JOIST_BIT_SLEEP		4u

struct joist_thread
{
  struct jg_waiter waiter;	/* in the futex queues; its notif: below */
  uint32_t self;		/* its own thread capability */
  uint32_t recv;		/* its notification (to wait on), 0 none yet */
  uint32_t sig_gsync;		/* capabilities to signal it, bits 0 and 1 */
  uint32_t sig_htl;
  uint32_t sc;			/* its scheduling context, 0 none of its own */
  uint32_t ipc_on;		/* 1 once it has registered ipc_buf itself */
  uint64_t kept;		/* bits a wait of the other kind took */
  uint64_t *ipc_buf;		/* its IPC buffer (joist-mig.c) */
  /* A server thread that must wait while it serves a call (a lock, a
     condition, a sleep): the kernel refuses a Wait while the thread holds
     the call's reply capability (I10), so the capability is moved into
     the thread's reply object first (reply_save, made at its first need)
     and the answer goes through it (reply_saved, joist-mig.c).  */
  uint32_t reply_obj;		/* its reply object, 0 none yet */
  uint32_t reply_saved;		/* 1: the call it serves is answered there */
};

/* The first thread's record until its TLS is set up (tls.h).  */
extern struct joist_thread __joist_thread0;

/* T's notification and the capabilities to signal it, its thread
   capability SELF: 0, or a Mach error (KERN_RESOURCE_SHORTAGE).  */
extern int __joist_thread_make (struct joist_thread *t, uint32_t self);
/* T's notification and capabilities given back.  */
extern void __joist_thread_free (struct joist_thread *t);
/* Wait for BIT on T's notification (T the calling thread's) for at most
   TIMEOUT nanoseconds (JG_FOREVER: no limit): 0 when it came, 1 when the
   time ran out, 2 when the wait was aborted (thread_abort, or
   thread_suspend, which aborts first: Mach's MACH_RCV_INTERRUPTED; the
   caller waits again, or returns EINTR).  A server thread serving a call
   saves its reply first (reply_obj above).  */
extern int __joist_thread_wait (struct joist_thread *t, uint64_t bit,
				uint64_t timeout);
/* Signal BIT to T.  */
extern void __joist_thread_signal (const struct joist_thread *t,
				   uint64_t bit);
/* T's reply object made (T the calling thread, holding no reply: its
   waits are allowed), if it has none: 0, or -1.  A server thread's plain
   receive does it (joist-mig.c's server_ipc), so that reply_save, which
   runs inside a refused wait, never makes one: making it takes the names
   lock, and a wait for that lock inside a wait for it (on the same
   thread's futex queue entry) would lose the first wait's wake-up.  */
extern int __joist_reply_obj_make (struct joist_thread *t);
/* The futex queues' table made: before the second thread exists (htl's
   pt-thread-alloc); until then no lock can be contended.  */
extern int __joist_sync_init (void);

#endif
