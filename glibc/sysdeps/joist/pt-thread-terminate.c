/* End a thread, htl on Joist (Phase 5 step 7, the design's item 7): its
   notification and IPC buffer given back, its scheduling context kept for the next
   thread (it can't delete its own and go on: joist-sync.c), then its
   thread capability deleted, which ends it.  Its stack isn't freed: Mach's
   thread_terminate_release would, after the thread is gone; htl leaks it
   too where the kernel can't (a fixed pool of threads bounds it, as
   libports' will be: the design's step 8).
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

void
__pthread_thread_terminate (struct __pthread *thread)
{
  thread_t kernel_thread = thread->kernel_thread;
  int self = kernel_thread == __mach_thread_self ();

  /* The kernel thread won't be there any more.  */
  thread->kernel_thread = MACH_PORT_DEAD;
  __pthread_dealloc (thread);

  /* After __pthread_dealloc_finish, pthread_create may reuse THREAD and
     free its TCB: when THREAD is the caller, its own TLS, which a lock's
     wait needs (its record).  So what takes a lock is done before, and
     only the kernel's calls after.  pthread_create's failure path has
     freed the TCB already (pt-thread-start.c the record's names).  */
  if (thread->tcb != NULL)
    {
      struct joist_thread *rec = &thread->tcb->joist;
      if (rec->sc != 0)
	__joist_sc_keep (rec->sc);
      rec->sc = 0;
      __joist_thread_free (rec);
      /* Its IPC buffer: the calls left (the kernel's) carry no words.  */
      __joist_ipc_free (rec);
    }
  kern_return_t err = __joist_thread_forget (kernel_thread);

  __pthread_dealloc_finish (thread);

  if (err == KERN_SUCCESS)
    err = __joist_thread_end (kernel_thread, self);
  /* Only another thread's ends here.  */
  assert (!self);
  assert_perror (err);
}
