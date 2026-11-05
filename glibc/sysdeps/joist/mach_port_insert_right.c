/* mach_port_insert_right on Joist (Phase 5 step 7): MAKE_SEND on a receive
   right, COPY_SEND or MOVE_SEND of a send right.
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
#include "joist-ports.h"

kern_return_t
__mach_port_insert_right (mach_port_t task,
                          mach_port_name_t name,
                          mach_port_t poly,
                          mach_msg_type_name_t polyPoly)
{
  return JOIST_PORTS_CALL (task, __jn_insert_right (t, name, poly, polyPoly));
}
weak_alias (__mach_port_insert_right, mach_port_insert_right)
libc_hidden_def (__mach_port_insert_right)
