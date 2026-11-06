/* Start a thread, htl on Joist (Phase 5 step 7, the design's item 7): a
   scheduling context of its own, bound to it; it runs from here.  The
   main thread runs already.  A context a terminated thread left is used
   again first (pt-thread-terminate.c).
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

#include <assert.h>
#include <errno.h>
#include <mach.h>
#include <pt-internal.h>
#include <tls.h>
#include "joist-ports.h"
#include "joist-sys.h"

#define JOIST_SYS_SC_CREATE	50
#define JOIST_SYS_SC_BIND	51

int
__pthread_thread_start (struct __pthread *thread)
{
  static int do_start;

  if (!do_start)
    {
      /* The main thread is already running: do nothing.  */
      assert (__pthread_total == 1);
      do_start = 1;
      return 0;
    }
  /* A context a terminated thread left, if one is free by now (its
     thread may not have finished dying: then a new one).  */
  uint32_t sc = __joist_sc_reuse ();
  if (sc != 0
      && __joist_sys (JOIST_SYS_SC_BIND, sc, thread->kernel_thread, 0, 0, NULL)
	 == JOIST_OK)
    {
      thread->tcb->joist.sc = sc;
      return 0;
    }
  if (sc != 0)
    __joist_sc_keep (sc);
  sc = 0;
    {
      struct jn_table *t = __joist_ports_lock ();
      int r = __jn_pinned (t, &sc);
      __joist_ports_unlock ();
      if (r != JN_OK)
	return EAGAIN;
      if (__joist_sys (JOIST_SYS_SC_CREATE, sc, JOIST_THREAD_PRIO,
		       JOIST_THREAD_PERIOD, JOIST_THREAD_PERIOD, NULL)
	  != JOIST_OK)
	{
	  t = __joist_ports_lock ();
	  __jn_unpin (t, sc);
	  __joist_ports_unlock ();
	  return EAGAIN;
	}
    }
  thread->tcb->joist.sc = sc;
  if (__joist_sys (JOIST_SYS_SC_BIND, sc, thread->kernel_thread, 0, 0, NULL)
      != JOIST_OK)
    {
      __joist_sc_keep (sc);
      thread->tcb->joist.sc = 0;
      return EAGAIN;
    }
  return 0;
}
libc_hidden_def (__pthread_thread_start)
