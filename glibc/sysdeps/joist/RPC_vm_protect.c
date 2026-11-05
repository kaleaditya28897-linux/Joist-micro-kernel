/* vm_protect on Joist (Phase 5 step 7).
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

/* The current protection only: the table keeps no maximum, so a call
   setting one is refused (KERN_INVALID_ARGUMENT; glibc never makes
   one).  */

#include "joist-mem.h"

kern_return_t
__vm_protect (mach_port_t target_task, vm_address_t address, vm_size_t size,
	      boolean_t set_maximum, vm_prot_t new_protection)
{
  if (set_maximum)
    return KERN_INVALID_ARGUMENT;
  return JOIST_VM_CALL (target_task,
			__jv_vm_protect (t, address, size, new_protection));
}
weak_alias (__vm_protect, vm_protect)
