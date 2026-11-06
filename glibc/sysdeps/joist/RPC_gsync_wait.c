/* gsync_wait (and, through hurd/RPC_gsync_wait_intr.c, gsync_wait_intr: signals are Phase 7) on Joist (Phase 5 step 7): the futex queues, joist-sync.c.
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
__gsync_wait (mach_port_t task, vm_address_t addr, unsigned val1,
	      unsigned val2, natural_t msec, int flags)
{
  return __joist_gsync_wait_call (task, addr, val1, val2, msec, flags);
}
weak_alias (__gsync_wait, gsync_wait)
