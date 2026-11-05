/* mach_port_set_protected_payload on Joist (Phase 5 step 7): the badge a
   receive right's messages will arrive with; refused once it has one (0005).
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

#include "joist-ports.h"

kern_return_t
__mach_port_set_protected_payload (mach_port_t task,
                                   mach_port_name_t name,
                                   rpc_uintptr_t payload)
{
  return JOIST_PORTS_CALL (task, __jn_set_payload (t, name, payload));
}
weak_alias (__mach_port_set_protected_payload, mach_port_set_protected_payload)
