/* task_terminate on Joist (Phase 5 step 7): this task destroyed, through
   its own capability (decision 0004); another task's is Phase 7's (proc).
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

#include <mach.h>
#include "joist-sys.h"
#include "joist-thread.h"

#define JOIST_SYS_TASK_DESTROY	31

kern_return_t
__task_terminate (task_t task)
{
  if (task != __mach_task_self ())
    return KERN_INVALID_ARGUMENT;
  (void) __joist_sys (JOIST_SYS_TASK_DESTROY, JOIST_SLOT_TASK, 0, 0, 0, NULL);
  /* Not reached: the calling thread died with its task.  */
  __builtin_trap ();
}
weak_alias (__task_terminate, task_terminate)
libc_hidden_def (__task_terminate)
