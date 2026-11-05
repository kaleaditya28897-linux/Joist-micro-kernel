/* vm_map on Joist (Phase 5 step 7): anonymous memory only.
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

/* Anonymous memory (MEMORY_OBJECT null) is vm_allocate's with an
   alignment (MASK, a power of 2 less one) and a protection; a memory
   object is refused (decision 0007: Phase 7), as the design's item 10
   lists.  The maximum protection, the copy flag and the inheritance are
   not kept: a task's memory goes with it alone until fork (Phase 7).  */

#include <mach-shortcuts-hidden.h>
#include "joist-mem.h"

kern_return_t
__vm_map (mach_port_t target_task, vm_address_t *address, vm_size_t size,
	  vm_address_t mask, boolean_t anywhere, mach_port_t memory_object,
	  vm_offset_t offset, boolean_t copy, vm_prot_t cur_protection,
	  vm_prot_t max_protection, vm_inherit_t inheritance)
{
  if (memory_object != MACH_PORT_NULL)
    return KERN_INVALID_ARGUMENT;
  uint64_t a = *address;
  kern_return_t r = JOIST_VM_CALL (target_task,
				   __jv_allocate (t, &a, size, mask, anywhere,
						  cur_protection));
  if (r == KERN_SUCCESS)
    *address = a;
  return r;
}
weak_alias (__vm_map, vm_map)
libc_hidden_def (__vm_map)
