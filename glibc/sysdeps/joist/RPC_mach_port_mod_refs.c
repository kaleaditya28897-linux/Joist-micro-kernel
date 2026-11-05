/* mach_port_mod_refs on Joist (Phase 5 step 7): user references, counted in
   libc.
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
__mach_port_mod_refs (mach_port_t task,
                      mach_port_name_t name,
                      mach_port_right_t right,
                      mach_port_delta_t delta)
{
  return JOIST_PORTS_CALL (task, __jn_mod_refs (t, name, right, delta));
}
weak_alias (__mach_port_mod_refs, mach_port_mod_refs)
