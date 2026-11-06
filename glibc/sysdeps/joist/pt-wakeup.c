/* Wake a thread, htl on Joist (Phase 5 step 7): bit 1 signalled on its
   notification (pt-block.c).
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

void
__pthread_wakeup (struct __pthread *thread)
{
  __joist_thread_signal (&thread->tcb->joist, JOIST_BIT_HTL);
}
libc_hidden_def (__pthread_wakeup)
