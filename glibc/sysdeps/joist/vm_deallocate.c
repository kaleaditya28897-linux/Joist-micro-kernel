/* vm_deallocate on Joist (Phase 5 step 7).
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
__vm_deallocate (mach_port_t target_task, vm_address_t address,
		 vm_size_t size)
{
  return JOIST_VM_CALL (target_task, __jv_deallocate (t, address, size));
}
weak_alias (__vm_deallocate, vm_deallocate)
libc_hidden_def (__vm_deallocate)
