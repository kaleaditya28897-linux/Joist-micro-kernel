/* thread_create on Joist (Phase 5 step 7, the design's item 7): a thread
   spawned in this task with no scheduling context, so it doesn't run (a
   passive thread runs only on lent time) until htl gives it one
   (pt-thread-start.c).  Its capability takes a pinned name.
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
#include "joist-ports.h"
#include "joist-sys.h"
#include "joist-thread.h"

#define JOIST_SYS_THREAD_SPAWN	32

kern_return_t
__thread_create (task_t task, thread_t *child)
{
  if (task != __mach_task_self ())
    return KERN_INVALID_ARGUMENT;	/* another task's: Phase 7 (fork) */
  uint32_t name;
  struct jn_table *t = __joist_ports_lock ();
  int r = __jn_pinned (t, &name);
  __joist_ports_unlock ();
  if (r != JN_OK)
    return KERN_RESOURCE_SHORTAGE;
  if (__joist_sys (JOIST_SYS_THREAD_SPAWN, JOIST_SLOT_TASK, name, 0, 0, NULL)
      != JOIST_OK)
    {
      t = __joist_ports_lock ();
      __jn_unpin (t, name);
      __joist_ports_unlock ();
      return KERN_RESOURCE_SHORTAGE;
    }
  *child = name;
  return KERN_SUCCESS;
}
weak_alias (__thread_create, thread_create)
