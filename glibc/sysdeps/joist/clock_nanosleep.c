/* clock_nanosleep - high-resolution sleep with specifiable clock.
   Copyright (C) 2002-2026 Free Software Foundation, Inc.
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

/* Joist's copy of sysdeps/mach/clock_nanosleep.c (Phase 5 step 7, the
   design's item 9), changed in nanosleep_call: the wait is on the calling
   thread's notification (joist-thread.h), not mach_msg.  */

#include <errno.h>
#include <mach.h>
#include <tls.h>
#include "joist-gsync.h"
#include <time.h>
#include <unistd.h>
#include <posix-timer.h>
#include <shlib-compat.h>
#include <sysdep-cancel.h>

/* Joist: no mach_msg (the design's item 4), so no receive with a timeout
   on a reply port: the calling thread waits on its own notification for a
   bit no one signals (JOIST_BIT_SLEEP), so only the time running out or
   an abort (thread_abort, Mach's MACH_RCV_INTERRUPTED) ends the wait; a
   bit of another kind seen meanwhile is kept for its wait.  */
static int
nanosleep_call (clockid_t clock_id, const struct timespec *req, struct timespec *rem)
{
  struct joist_thread *t = __joist_thread_self ();
  struct timespec before;

  if (t->recv == 0 && __joist_thread_make (t, t->self) != 0)
    return ENOMEM;

  /* Time_t's seconds can be more than 64 bits of nanoseconds: no limit.  */
  uint64_t ns = JG_FOREVER;
  if ((uint64_t) req->tv_sec < (JG_FOREVER - 1) / 1000000000u)
    ns = (uint64_t) req->tv_sec * 1000000000u + (uint64_t) req->tv_nsec;

  if (rem != NULL)
    __clock_gettime (clock_id, &before);

  int cancel_oldtype = LIBC_CANCEL_ASYNC();
  int r = ns == 0 ? 1 : __joist_thread_wait (t, JOIST_BIT_SLEEP, ns);
  LIBC_CANCEL_RESET (cancel_oldtype);

  if (r == 2)
    {
      if (rem != NULL)
	{
	  struct timespec after, elapsed;
	  __clock_gettime (clock_id, &after);
	  timespec_sub (&elapsed, &after, &before);
	  timespec_sub (rem, req, &elapsed);
	}

      return EINTR;
    }

  return 0;
}

int
__clock_nanosleep (clockid_t clock_id, int flags, const struct timespec *req,
		   struct timespec *rem)
{
  if ((clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)
      || req->tv_sec < 0
      || !valid_nanoseconds (req->tv_nsec)
      || (flags != 0 && flags != TIMER_ABSTIME))
    return EINVAL;

  struct timespec now;

  /* If we got an absolute time, remap it.  */
  if (flags == TIMER_ABSTIME)
    {
      long int nsec;
      long int sec;

      /* Make sure we use safe data types.  */
      assert (sizeof (sec) >= sizeof (now.tv_sec));

      /* Get the current time for this clock.  */
      if (__clock_gettime (clock_id, &now) != 0)
	return errno;

      /* Compute the difference.  */
      nsec = req->tv_nsec - now.tv_nsec;
      sec = req->tv_sec - now.tv_sec - (nsec < 0);
      if (sec < 0)
	/* The time has already elapsed.  */
	return 0;

      now.tv_sec = sec;
      now.tv_nsec = nsec + (nsec < 0 ? 1000000000 : 0);

      /* From now on this is our time.  */
      req = &now;

      /* Make sure we are not modifying the struct pointed to by REM.  */
      rem = NULL;
    }

  return nanosleep_call (clock_id, req, rem);
}
libc_hidden_def (__clock_nanosleep)
versioned_symbol (libc, __clock_nanosleep, clock_nanosleep, GLIBC_2_17);
/* clock_nanosleep moved to libc in version 2.17;
   old binaries may expect the symbol version it had in librt.  */
#if SHLIB_COMPAT (libc, GLIBC_2_2, GLIBC_2_17)
strong_alias (__clock_nanosleep, __clock_nanosleep_2);
compat_symbol (libc, __clock_nanosleep_2, clock_nanosleep, GLIBC_2_2);
#endif
