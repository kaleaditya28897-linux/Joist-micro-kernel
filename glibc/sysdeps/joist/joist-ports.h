/* libc's port names on Joist: the table and its lock (Phase 5 step 7).
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

/* The task's one name table (joist-names.h), under one lock: the
   mach_port_* calls take it for the whole call, kernel calls included
   (none of them blocks).  Names below JOIST_FIXED_NAMES are the slots
   start-up fills (the design's item 8).  Only the task's own names are
   here: a call naming another task is refused with
   MACH_SEND_INVALID_DEST, as for a task port that names nothing (another
   task's names are Phase 7's, with fork).  */

#ifndef _JOIST_PORTS_H
#define _JOIST_PORTS_H 1

#include <mach.h>
#include "joist-names.h"

#define JOIST_FIXED_NAMES 16u

/* The table, locked (made on first use).  A program has one table: in
   ld.so if it is dynamic (ld.so runs first; libc.so and libmachuser use
   its table through these and the __jn_* calls, exported at
   GLIBC_PRIVATE: Versions), in libc.a if it is static.  */
extern struct jn_table *__joist_ports_lock (void);
extern void __joist_ports_unlock (void);
rtld_hidden_proto (__joist_ports_lock)
rtld_hidden_proto (__joist_ports_unlock)
rtld_hidden_proto (__jn_allocate)
rtld_hidden_proto (__jn_deallocate)
rtld_hidden_proto (__jn_destroy)
rtld_hidden_proto (__jn_get_refs)
rtld_hidden_proto (__jn_insert_right)
rtld_hidden_proto (__jn_mod_refs)
rtld_hidden_proto (__jn_move_member)
rtld_hidden_proto (__jn_reply_name)
rtld_hidden_proto (__jn_set_payload)
rtld_hidden_proto (__jn_type)

/* Whether TASK is this task.  */
#define JOIST_PORTS_SELF(task) ((task) == __mach_task_self ())

/* The body of a mach_port_* call on TASK: CALL, with T the table.  */
#define JOIST_PORTS_CALL(task, call)				\
  ({								\
    kern_return_t __r = MACH_SEND_INVALID_DEST;			\
    if (JOIST_PORTS_SELF (task))				\
      {								\
	struct jn_table *t = __joist_ports_lock ();		\
	__r = (call);						\
	__joist_ports_unlock ();				\
      }								\
    __r;							\
  })

#endif
