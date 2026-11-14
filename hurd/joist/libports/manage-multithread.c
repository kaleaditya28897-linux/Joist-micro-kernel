/*
   Copyright (C) 1995, 1996, 1997 Free Software Foundation, Inc.
   Written by Michael I. Bushnell.

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
   Hurd's threads on Joist messages, each answer sent with its thread's
   next receive (ReplyRecv), a request served by _ports_joist_serve
   (joist.c).  A thread is still made when the last listening one takes a
   request, but no more than the bucket's pool (ports_set_pool_size): past
   it, requests wait in the kernel's queue, as the plan's choice 6 has it.
   The priority adjustment is gone (Phase 5 has no thread priorities to
   ask for), and so is the cancel threshold (no sequence numbers).  Without
   GCC's nested functions (clang builds Joist's libraries), the loop's
   state is a struct.  */

#include "ports.h"
#include "joist-server.h"
#include <errno.h>
#include <sched.h>
#include <stdio.h>

#define STACK_SIZE (64 * 1024)

struct manager
{
  struct port_bucket *bucket;
  ports_demuxer_type demuxer;
  int thread_timeout, global_timeout;
  void (*hook) (void);
  pthread_attr_t attr;
  /* totalthreads is the number of total threads created.  nreqthreads
     is the number of threads not currently servicing any client.  The
     initial values account for the main thread.  */
  unsigned int totalthreads, nreqthreads;
};

static void *serve (struct manager *m, int master);

static void *
thread_function (void *arg)
{
  return serve (arg, 0);
}

/* The last listening thread has taken a request: another, if the pool
   has room.  */
static void
another (struct manager *m)
{
  unsigned int total = __atomic_load_n (&m->totalthreads, __ATOMIC_RELAXED);
  do
    if (total >= m->bucket->joist_pool)
      return;
  while (!__atomic_compare_exchange_n (&m->totalthreads, &total, total + 1, 0,
				       __ATOMIC_RELAXED, __ATOMIC_RELAXED));
  __atomic_add_fetch (&m->nreqthreads, 1, __ATOMIC_RELAXED);

  pthread_t pthread_id;
  error_t err = pthread_create (&pthread_id, &m->attr, thread_function, m);
  if (!err)
    pthread_detach (pthread_id);
  else
    {
      __atomic_sub_fetch (&m->totalthreads, 1, __ATOMIC_RELAXED);
      __atomic_sub_fetch (&m->nreqthreads, 1, __ATOMIC_RELAXED);
      /* There is not much we can do at this point.  The code
	 and design of the Hurd servers just don't handle
	 thread creation failure.  */
      errno = err;
      perror ("pthread_create");
    }
}

static void *
serve (struct manager *m, int master)
{
  struct port_bucket *bucket = m->bucket;
  struct ports_thread thread;
  PORTS_JOIST_MSGS (in, out);
  int answer = 0;
  error_t err;

  if (m->hook)
    (*m->hook) ();

  int timeout = master ? m->global_timeout : m->thread_timeout;
  uint64_t ns = (uint64_t) (timeout ? timeout : 10 * 1000) * 1000000;

  _ports_thread_online (&bucket->threadpool, &thread);

 startover:
  for (;;)
    {
      err = answer
	? __joist_mig_reply_receive (&out, bucket->portset, &in, ns)
	: __joist_mig_receive (bucket->portset, &in, ns);
      answer = 0;
      _ports_thread_quiescent (&bucket->threadpool, &thread);
      if (err == MACH_RCV_TIMED_OUT)
	{
	  if (timeout)
	    break;
	  continue;
	}
      if (err == MACH_RCV_INVALID_NAME)
	break;			/* (the set gone: nothing left to serve) */
      if (err)
	continue;		/* the answer refused; the next request */

      if (__atomic_sub_fetch (&m->nreqthreads, 1, __ATOMIC_RELAXED) == 0)
	/* No thread would be listening for requests, spawn one. */
	another (m);
      answer = _ports_joist_serve (bucket, m->demuxer, &in, &out);
      __atomic_add_fetch (&m->nreqthreads, 1, __ATOMIC_RELAXED);
      _ports_thread_quiescent (&bucket->threadpool, &thread);
    }

  if (master)
    {
      if (__atomic_load_n (&m->totalthreads, __ATOMIC_RELAXED) != 1
	  && err != MACH_RCV_INVALID_NAME)
	goto startover;
      /* (with the set gone, the others leave too; then M can go) */
      while (__atomic_load_n (&m->totalthreads, __ATOMIC_RELAXED) != 1)
	sched_yield ();
    }
  else
    {
      if (err != MACH_RCV_INVALID_NAME
	  && __atomic_sub_fetch (&m->nreqthreads, 1, __ATOMIC_RELAXED) == 0)
	{
	  /* No other thread is listening for requests, continue. */
	  __atomic_add_fetch (&m->nreqthreads, 1, __ATOMIC_RELAXED);
	  goto startover;
	}
      if (err == MACH_RCV_INVALID_NAME)
	__atomic_sub_fetch (&m->nreqthreads, 1, __ATOMIC_RELAXED);
      __atomic_sub_fetch (&m->totalthreads, 1, __ATOMIC_RELAXED);
    }
  _ports_thread_offline (&bucket->threadpool, &thread);
  return NULL;
}

void
ports_manage_port_operations_multithread (struct port_bucket *bucket,
					  ports_demuxer_type demuxer,
					  int thread_timeout,
					  int global_timeout,
					  void (*hook)(void))
{
  struct manager m = {
    .bucket = bucket, .demuxer = demuxer,
    .thread_timeout = thread_timeout, .hook = hook,
    .totalthreads = 1, .nreqthreads = 1,
  };

  pthread_attr_init (&m.attr);
  pthread_attr_setstacksize (&m.attr, STACK_SIZE);

  /* XXX It is currently unsafe for most servers to terminate based on
     inactivity because a request may arrive after a server has started
     shutting down, causing the client to receive an error.  Prevent the
     master thread from going away.  */
  m.global_timeout = 0;
  (void) global_timeout;

  serve (&m, 1);
  pthread_attr_destroy (&m.attr);
}
