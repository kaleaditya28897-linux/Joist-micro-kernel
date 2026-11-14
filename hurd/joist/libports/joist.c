/* joist.c - libports on Joist (Phase 5 step 8; the owner's design of
   2026-11-12): ports found by badge, a request served, no-senders
   collected by a thread a bucket, a server thread's RPC interrupted.
   Copyright (C) 2026 Aditya Kale.

   This file is part of Joist's libports, from the GNU Hurd's.

   It is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 2, or (at your option) any later
   version.

   It is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.  */

#include "ports.h"
#include "joist-server.h"
#include <errno.h>
#include <stddef.h>
#include <pthread.h>
#include <hurd/ihash.h>
#include <hurd/signal.h>
#include <mach/mig_errors.h>

/* ---- the table of live ports by badge ----

   A port's badge is a serial number, its protected payload
   (create-internal.c), never reused: Joist keeps a badge on every send
   capability handed out, so a request can come for a port gone since,
   and the Hurd's way, the port_info's address as the payload, would then
   be a pointer to freed memory.  Under _ports_htable_lock, as libports'
   other tables.  */

static struct hurd_ihash badges
  = HURD_IHASH_INITIALIZER (offsetof (struct port_info, joist_badge_entry));
static uintptr_t last_badge;

error_t
_ports_joist_badge_add (struct port_info *pi)
{
  pi->joist_badge = __atomic_add_fetch (&last_badge, 1, __ATOMIC_RELAXED);
  return hurd_ihash_add (&badges, pi->joist_badge, pi);
}

void
_ports_joist_badge_remove (struct port_info *pi)
{
  hurd_ihash_locp_remove (&badges, pi->joist_badge_entry);
}

void *
ports_lookup_payload (struct port_bucket *bucket, uintptr_t payload,
		      struct port_class *class)
{
  pthread_rwlock_rdlock (&_ports_htable_lock);
  struct port_info *pi = hurd_ihash_find (&badges, payload);
  if (pi != NULL && bucket != NULL && pi->bucket != bucket)
    pi = NULL;
  if (pi != NULL && class != NULL && pi->class != class)
    pi = NULL;
  if (pi != NULL)
    refcounts_unsafe_ref (&pi->refcounts, NULL);
  pthread_rwlock_unlock (&_ports_htable_lock);
  return pi;
}

mach_port_t
ports_payload_get_name (uintptr_t payload)
{
  pthread_rwlock_rdlock (&_ports_htable_lock);
  struct port_info *pi = hurd_ihash_find (&badges, payload);
  mach_port_t name = pi != NULL ? pi->port_right : MACH_PORT_NULL;
  pthread_rwlock_unlock (&_ports_htable_lock);
  return name;
}

/* ---- one request ---- */

/* A failed request's rights: dropped, as mach_msg_server's
   mach_msg_destroy does (a receive right by its reference).  */
static void
drop_rights (struct joist_mig_msg *in)
{
  for (uint32_t i = 0; i < in->ncaps; i++)
    if (MACH_PORT_VALID (in->cap[i])
	&& mach_port_deallocate (mach_task_self (), in->cap[i]) != 0)
      (void) mach_port_mod_refs (mach_task_self (), in->cap[i],
				 MACH_PORT_RIGHT_RECEIVE, -1);
  in->ncaps = 0;
}

int
_ports_joist_serve (struct port_bucket *bucket, ports_demuxer_type demuxer,
		    struct joist_mig_msg *in, struct joist_mig_msg *out)
{
  out->r[0] = (uint32_t) MIG_BAD_ID;
  out->r[1] = out->r[2] = out->r[3] = 0;
  out->ncaps = 0;
  out->len = 0;
  struct port_info *pi = ports_lookup_payload (bucket, in->badge, NULL);
  if (pi == NULL)
    /* A port gone (or never this bucket's): as the Hurd's loops answer
       a port they don't serve.  */
    out->r[0] = (uint32_t) EOPNOTSUPP;
  else
    {
      /* (No cancel threshold: Joist's messages have no sequence number,
	 so ports_interrupt_self_on_notification's early cancel isn't
	 seen here; the RPC's own interrupt_operation still is.)  */
      struct rpc_info link;
      error_t err = ports_begin_rpc (pi, joist_mig_code (in), &link);
      if (err)
	out->r[0] = (uint32_t) err;
      else
	{
	  (void) demuxer (in, out);
	  ports_end_rpc (pi, &link);
	}
      ports_port_deref (pi);
    }
  int32_t ret = (int32_t) (uint32_t) out->r[0];
  if (ret == MIG_NO_REPLY)
    return 0;
  if (ret != 0)
    drop_rights (in);
  return 1;
}

void
ports_set_pool_size (struct port_bucket *bucket, unsigned int n)
{
  bucket->joist_pool = n > 0 ? n : 1;
}

/* ---- no-senders ----

   The kernel queues a port's badge on its bucket's endpoint when its last
   sender goes, and signals the bucket's notification (I4); a mint of that
   badge before the queue is read takes the firing back (I4, amended
   2026-11-12).  A send right ports_get_right hands out is made later, as
   its reply is sent, so the firing comes with glibc's make-send count as
   it was taken: the Hurd's ports_no_senders then ignores it if a right
   was handed out since (the port's own count past it), as on Mach.  */

static void *
notifier (void *arg)
{
  struct port_bucket *bucket = arg;
  for (;;)
    {
      int err = __joist_nosenders_wait (bucket->joist_nosenders);
      if (err != 0 && err != EINTR)
	break;
      uint64_t badge;
      mach_port_mscount_t mscount;
      while (__joist_nosenders_next (bucket->portset, &badge, &mscount) == 0
	     && badge != 0)
	{
	  struct port_info *pi = ports_lookup_payload (bucket, badge, NULL);
	  if (pi != NULL)
	    {
	      ports_no_senders (pi, mscount);
	      ports_port_deref (pi);
	    }
	}
    }
  return NULL;
}

error_t
_ports_joist_nosenders_start (struct port_bucket *bucket)
{
  error_t err = __joist_nosenders_bind (bucket->portset,
					&bucket->joist_nosenders);
  if (err)
    return err;
  pthread_t t;
  err = pthread_create (&t, NULL, notifier, bucket);
  if (err == 0)
    pthread_detach (t);
  return err;
}

/* ---- interrupting a server thread's RPC ----

   Another thread's state is refused in Phase 5 (step 7's item 10), so the
   Hurd's hurd_thread_cancel, which aborts a call the thread is blocked
   in, becomes its cancel flag alone: the RPC sees it at its next
   hurd_check_cancel (or ports_self_interrupted), and a call it is
   blocked in runs to its end.  Phase 7 brings the abort, once Joist's own
   locks are critical sections.  */

error_t
_ports_joist_cancel (thread_t thread)
{
  struct hurd_sigstate *ss = _hurd_thread_sigstate (thread);
  if (ss == NULL)
    return ESRCH;
  _hurd_sigstate_lock (ss);
  ss->cancel = 1;
  _hurd_sigstate_unlock (ss);
  return 0;
}
