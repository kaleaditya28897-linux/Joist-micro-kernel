/* The task's address reservations in libc (Phase 5 step 7).
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

/* The task's one reservation table (joist-vm.h) under one lock, held for
   the whole of a vm_* call, mm's Call included: the table's check, mm's
   change and the table's change are one step to the other threads.  Only
   the task's own memory: a call naming another task is refused with
   MACH_SEND_INVALID_DEST (another task's memory is Phase 7's, with
   fork), as the mach_port_* calls do.  */

#ifndef _JOIST_MEM_H
#define _JOIST_MEM_H 1

#include <mach.h>
#include "joist-vm.h"

/* The table, locked.  A program has one: in ld.so if it is dynamic, in
   libc.a if it is static (as the name table: joist-ports.h).  */
extern struct jv_table *__joist_vm_lock (void);
extern void __joist_vm_unlock (void);
rtld_hidden_proto (__joist_vm_lock)
rtld_hidden_proto (__joist_vm_unlock)
rtld_hidden_proto (__jv_allocate)
rtld_hidden_proto (__jv_deallocate)
rtld_hidden_proto (__jv_vm_protect)
rtld_hidden_proto (__jv_vm_region)

/* The body of a vm_* call on TASK: CALL, with T the table.  */
#define JOIST_VM_CALL(task, call)				\
  ({								\
    kern_return_t __r = MACH_SEND_INVALID_DEST;			\
    if ((task) == __mach_task_self ())				\
      {								\
	struct jv_table *t = __joist_vm_lock ();		\
	__r = (call);						\
	__joist_vm_unlock ();					\
      }								\
    __r;							\
  })

#endif
