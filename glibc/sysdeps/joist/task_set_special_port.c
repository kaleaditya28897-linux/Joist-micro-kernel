/* task_set_special_port on Joist (Phase 5 step 7): joist-special.c.
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
__task_set_special_port (task_t task, int which, mach_port_t port)
{
  if (task != __mach_task_self () || which != TASK_BOOTSTRAP_PORT)
    return KERN_INVALID_ARGUMENT;
  __joist_bootstrap = port;
  return KERN_SUCCESS;
}
weak_alias (__task_set_special_port, task_set_special_port)
libc_hidden_def (__task_set_special_port)
