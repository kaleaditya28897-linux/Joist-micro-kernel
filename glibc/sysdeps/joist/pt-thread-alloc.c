/* A thread's kernel resources, htl on Joist (Phase 5 step 7, the design's
   item 7).  The first call is for the main thread, which exists: its
   notification is made now, if it hasn't been.  Every other is a new
   thread, which doesn't run until pt-thread-start.c gives it a scheduling
   context; before the first of them, the futex queues are made (no lock
   can have been contended yet).
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

int
__pthread_thread_alloc (struct __pthread *thread)
{
  static int do_create;

  if (!do_create)
    {
      assert (__pthread_total == 0);
      struct joist_thread *t = __joist_thread_self ();
      if (t->recv == 0 && __joist_thread_make (t, t->self) != 0)
	return EAGAIN;
      thread->kernel_thread = t->self;
      do_create = 1;
      return 0;
    }
  if (__joist_sync_init () != 0)
    return EAGAIN;
  if (__thread_create (__mach_task_self (), &thread->kernel_thread)
      != KERN_SUCCESS)
    return EAGAIN;
  return 0;
}
