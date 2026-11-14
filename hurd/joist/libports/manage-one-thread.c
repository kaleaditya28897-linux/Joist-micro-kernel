/* 
   Copyright (C) 1995, 1996 Free Software Foundation, Inc.
   Writtenb by Michael I. Bushnell.

   This file is part of the GNU Hurd.

   The GNU Hurd is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   The GNU Hurd is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. */

/* Joist's copy (Phase 5 step 8; the owner's design of 2026-11-12): the
   Hurd's loop on Joist messages, each answer sent with the next receive
   (ReplyRecv); a request served by _ports_joist_serve (joist.c).  */

#include "ports.h"
#include "joist-server.h"

void
ports_manage_port_operations_one_thread (struct port_bucket *bucket,
					 ports_demuxer_type demuxer,
					 int timeout)
{
  struct ports_thread thread;
  PORTS_JOIST_MSGS (in, out);
  int answer = 0;
  error_t err;

  /* XXX It is currently unsafe for most servers to terminate based on
     inactivity because a request may arrive after a server has
     started shutting down, causing the client to receive an error.
     Prevent the service loop from terminating by setting TIMEOUT to
     zero.  */
  timeout = 0;
  uint64_t ns = timeout ? (uint64_t) timeout * 1000000 : JOIST_FOREVER;

  _ports_thread_online (&bucket->threadpool, &thread);
  for (;;)
    {
      err = answer
	? __joist_mig_reply_receive (&out, bucket->portset, &in, ns)
	: __joist_mig_receive (bucket->portset, &in, ns);
      answer = 0;
      if (err == MACH_RCV_TIMED_OUT || err == MACH_RCV_INVALID_NAME)
	break;			/* (the set gone: nothing left to serve) */
      if (err)
	continue;		/* the answer refused; the next request */
      /* No need to check cancel threshold here, because in a single
	 threaded server the cancel is always handled in order.  */
      answer = _ports_joist_serve (bucket, demuxer, &in, &out);
      _ports_thread_quiescent (&bucket->threadpool, &thread);
    }
  _ports_thread_offline (&bucket->threadpool, &thread);
}
