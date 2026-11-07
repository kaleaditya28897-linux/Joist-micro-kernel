/* A task's special ports on Joist (Phase 5 step 7): the bootstrap port
   alone, kept by libc: the slot start-up fixed (joist-thread.h) until the
   Hurd sets another (_hurd_init does, with the one from the init ports).
   No kernel or exception ports: Joist has none (0013, 0027).
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

#if !(defined SHARED && IS_IN (libc))
mach_port_t __joist_bootstrap = JOIST_SLOT_BOOTSTRAP;
rtld_hidden_data_def (__joist_bootstrap)
#endif
