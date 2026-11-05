/* vm_allocate on Joist (Phase 5 step 7): pages from mm, placed by libc.
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

#include <mach-shortcuts-hidden.h>
#include "joist-mem.h"

kern_return_t
__vm_allocate (mach_port_t target_task, vm_address_t *address,
	       vm_size_t size, boolean_t anywhere)
{
  uint64_t a = *address;
  kern_return_t r = JOIST_VM_CALL (target_task,
				   __jv_allocate (t, &a, size, 0, anywhere,
						  VM_PROT_DEFAULT));
  if (r == KERN_SUCCESS)
    *address = a;
  return r;
}
weak_alias (__vm_allocate, vm_allocate)
libc_hidden_def (__vm_allocate)
