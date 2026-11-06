/* Block a thread until a time, htl on Joist (Phase 5 step 7): as pt-block.c,
   the wait bounded by ABSTIME on CLOCK_ID.
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

#include <errno.h>
#include <time.h>
#include <pt-internal.h>
#include <tls.h>
#include <stdio.h>

error_t
__pthread_timedblock (struct __pthread *thread,
		      const struct timespec *abstime, clockid_t clock_id)
{
  struct timespec now;
  if (__clock_gettime (clock_id, &now) != 0)
    return EINVAL;
  if (now.tv_sec > abstime->tv_sec
      || (now.tv_sec == abstime->tv_sec && now.tv_nsec >= abstime->tv_nsec))
    return ETIMEDOUT;
  uint64_t left = (uint64_t) (abstime->tv_sec - now.tv_sec) * 1000000000u
		  + (uint64_t) abstime->tv_nsec - (uint64_t) now.tv_nsec;
  struct joist_thread *t = &thread->tcb->joist;
  if (t->recv == 0 && __joist_thread_make (t, t->self) != 0)
    __libc_fatal ("Joist: no notification for a thread to block on\n");
  return __joist_thread_wait (t, JOIST_BIT_HTL, left) ? ETIMEDOUT : 0;
}
libc_hidden_def (__pthread_timedblock)
