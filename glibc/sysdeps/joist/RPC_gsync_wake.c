/* gsync_wake on Joist (Phase 5 step 7): the futex queues, joist-sync.c.
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
__gsync_wake (mach_port_t task, vm_address_t addr, unsigned val, int flags)
{
  return __joist_gsync_wake_call (task, addr, val, flags);
}
weak_alias (__gsync_wake, gsync_wake)
