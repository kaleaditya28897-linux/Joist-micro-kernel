/* vm_region on Joist (Phase 5 step 7): from libc's reservations.
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

/* The first reservation that ends past *ADDRESS.  What start-up mapped
   isn't one (joist-mem.c).  No memory objects, no sharing: the maximum
   protection is all, the inheritance Mach's default.  */

#include "joist-mem.h"

kern_return_t
__vm_region (mach_port_t target_task, vm_address_t *address,
	     vm_size_t *size, vm_prot_t *protection,
	     vm_prot_t *max_protection, vm_inherit_t *inheritance,
	     boolean_t *is_shared, mach_port_t *object_name,
	     vm_offset_t *offset)
{
  uint64_t a = *address, s;
  uint32_t p;
  kern_return_t r = JOIST_VM_CALL (target_task,
				   __jv_vm_region (t, &a, &s, &p));
  if (r != KERN_SUCCESS)
    return r;
  *address = a;
  *size = s;
  *protection = p;
  *max_protection = VM_PROT_ALL;
  *inheritance = VM_INHERIT_DEFAULT;
  *is_shared = 0;
  *object_name = MACH_PORT_NULL;
  *offset = 0;
  return KERN_SUCCESS;
}
weak_alias (__vm_region, vm_region)
