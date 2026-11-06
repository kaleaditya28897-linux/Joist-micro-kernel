/* sched_yield on Joist (Phase 5 step 7): the kernel's Yield (0014): the
   rest of this turn given up, behind the thread's equals.
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

#include <sched.h>
#include "joist-sys.h"

int
__sched_yield (void)
{
  (void) __joist_sys (54, 0, 0, 0, 0, NULL);
  return 0;
}
libc_hidden_def (__sched_yield)
weak_alias (__sched_yield, sched_yield)
