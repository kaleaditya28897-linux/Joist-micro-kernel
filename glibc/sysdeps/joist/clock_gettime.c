/* clock_gettime on Joist (Phase 5 step 7, the design's item 9): the
   kernel's clock call.  CLOCK_MONOTONIC (and CLOCK_BOOTTIME) is the time
   since boot; CLOCK_REALTIME is that plus an offset of 0 until a time
   server sets one (Phase 8).  CPU-time clocks are not kept yet: EINVAL.
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
#include <shlib-compat.h>
#include "joist-sys.h"

int
__clock_gettime (clockid_t clock_id, struct timespec *ts)
{
  switch (clock_id)
    {
    case CLOCK_MONOTONIC:
    case CLOCK_REALTIME:
#ifdef CLOCK_MONOTONIC_RAW
    case CLOCK_MONOTONIC_RAW:
#endif
#ifdef CLOCK_BOOTTIME
    case CLOCK_BOOTTIME:
#endif
      {
	uint64_t ns;
	__joist_clock (&ns, NULL);
	ts->tv_sec = (time_t) (ns / 1000000000u);
	ts->tv_nsec = (long int) (ns % 1000000000u);
	return 0;
      }
    default:
      __set_errno (EINVAL);
      return -1;
    }
}
libc_hidden_def (__clock_gettime)

versioned_symbol (libc, __clock_gettime, clock_gettime, GLIBC_2_17);
/* clock_gettime moved to libc in version 2.17;
   old binaries may expect the symbol version it had in librt.  */
#if SHLIB_COMPAT (libc, GLIBC_2_2, GLIBC_2_17)
strong_alias (__clock_gettime, __clock_gettime_2);
compat_symbol (libc, __clock_gettime_2, clock_gettime, GLIBC_2_2);
#endif

#if __TIMESIZE != 64
int
__clock_gettime64 (clockid_t clock_id, struct __timespec64 *ts64)
{
  struct timespec ts;
  int ret = __clock_gettime (clock_id, &ts);
  if (ret == 0)
    *ts64 = valid_timespec_to_timespec64 (ts);
  return ret;
}
libc_hidden_def (__clock_gettime64)
#endif
