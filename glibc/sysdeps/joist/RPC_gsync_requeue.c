/* gsync_requeue on Joist (Phase 5 step 7): the futex queues, joist-sync.c.
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

kern_return_t
__gsync_requeue (mach_port_t task, vm_address_t src_addr,
		 vm_address_t dst_addr, boolean_t wake_one, int flags)
{
  return __joist_gsync_requeue_call (task, src_addr, dst_addr, wake_one,
				     flags);
}
weak_alias (__gsync_requeue, gsync_requeue)
