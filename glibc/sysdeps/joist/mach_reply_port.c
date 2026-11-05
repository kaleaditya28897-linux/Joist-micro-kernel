/* mach_reply_port on Joist (Phase 5 step 7): a name that only stands for
   the reply capability each call has of its own (the design's item 4).
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

mach_port_t
__mach_reply_port (void)
{
  mach_port_t name = MACH_PORT_NULL;
  if (JOIST_PORTS_CALL (__mach_task_self (), __jn_reply_name (t, &name))
      != KERN_SUCCESS)
    return MACH_PORT_NULL;
  return name;
}
weak_alias (__mach_reply_port, mach_reply_port)
libc_hidden_def (__mach_reply_port)
