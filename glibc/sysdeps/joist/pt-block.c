/* Block a thread, htl on Joist (Phase 5 step 7): a wait for bit 1 on the
   thread's own notification (joist-thread.h), which pt-wakeup.c signals.
   A signal that comes before the wait is kept, by the notification's
   sticky bit or the record's kept bits.  Interrupting it is Phase 7's
   (signals): the _intr variants wait the same way.
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

#include <pt-internal.h>
#include <tls.h>
#include <stdio.h>

#ifndef RETTYPE
# define RETTYPE void
#endif
#ifndef RETURN
# define RETURN(val)
#endif

RETTYPE
__pthread_block (struct __pthread *thread)
{
  struct joist_thread *t = &thread->tcb->joist;
  if (t->recv == 0 && __joist_thread_make (t, t->self) != 0)
    __libc_fatal ("Joist: no notification for a thread to block on\n");
  (void) __joist_thread_wait (t, JOIST_BIT_HTL, JG_FOREVER);
  RETURN (0);
}
libc_hidden_def (__pthread_block)
