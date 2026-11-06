/* mach_host_self on Joist (Phase 5 step 7): a name that stands for the
   host, with no capability behind it (Joist has no host port, 0013):
   host_info answers on it from the kernel's clock call.
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
#include "joist-thread.h"

/* In parentheses: mach_init.h makes __mach_host_self a macro.  */
mach_port_t
(__mach_host_self) (void)
{
  return JOIST_SLOT_HOST;
}
weak_alias (__mach_host_self, mach_host_self)
libc_hidden_def (__mach_host_self)
