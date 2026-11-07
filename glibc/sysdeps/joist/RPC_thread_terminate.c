/* thread_terminate on Joist (Phase 5 step 7): the thread's capability
   deleted with its pinned name, the thread killed first where another
   capability may keep it (docs/abi.md).
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
#include <tls.h>
#include "joist-ports.h"
#include "joist-sys.h"

#define JOIST_SYS_THREAD_KILL	33

kern_return_t
__thread_terminate (thread_t thread)
{
  /* The name first: for the calling thread, nothing after the kill
     runs.  */
  struct jn_table *t = __joist_ports_lock ();
  uint32_t type;
  int known = __jn_type (t, thread, &type) == JN_OK;
  if (known)
    __jn_unpin (t, thread);
  __joist_ports_unlock ();
  if (!known)
    return KERN_INVALID_ARGUMENT;
  /* Deleting the last capability to a thread kills it, and a thread libc
     made has no other: for the calling thread nothing after the delete
     runs, and its slot is empty for the name freed above.  The first
     thread's capability may have another (the task's starter's: the
     Hurd's exec server deallocates its own, glibcroot keeps one to watch
     the program), and then a delete alone let it run on; so the first
     thread, and any other thread, is killed first.  The calling first
     thread's slot (JOIST_SLOT_THREAD, fixed: never a free name) stays
     filled.  */
  int self = thread == __joist_thread_self ()->self;
  if ((!self || thread == JOIST_SLOT_THREAD)
      && __joist_sys (JOIST_SYS_THREAD_KILL, thread, 0, 0, 0, NULL) != JOIST_OK)
    return KERN_INVALID_ARGUMENT;
  return __joist_sys (JOIST_SYS_CAP_DELETE, thread, 0, 0, 0, NULL) == JOIST_OK
	 ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}
weak_alias (__thread_terminate, thread_terminate)
