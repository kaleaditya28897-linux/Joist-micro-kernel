/* thread_set_state on Joist (Phase 5 step 7): i386_THREAD_STATE and
   i386_FSGS_BASE_STATE into the thread's register record (read, changed,
   written).  The calling thread's own record too, which is how its TLS
   base is set (tls.h): written back, the record resumes the thread where
   the read returned, so a flag in memory tells the second arrival there
   from the first (docs/abi.md, "A thread writing its own record").
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

#include "joist-state.h"
#include <tls.h>

kern_return_t
__thread_set_state (thread_t thread, int flavor, thread_state_t state,
		    mach_msg_type_number_t count)
{
  if (flavor == i386_THREAD_STATE ? count < i386_THREAD_STATE_COUNT
      : flavor == i386_FSGS_BASE_STATE ? count < i386_FSGS_BASE_STATE_COUNT
      : 1)
    return KERN_INVALID_ARGUMENT;
  if (flavor == i386_THREAD_STATE && thread == __joist_thread_self ()->self)
    return KERN_INVALID_ARGUMENT;	/* its own pc and sp: as Mach, no */
  struct joist_regs j;
  volatile int written = 0;
  kern_return_t r = joist_regs_read (thread, &j);
  if (r != KERN_SUCCESS || written)
    return r;		/* the second arrival: done (see above) */
  written = 1;
  if (flavor == i386_THREAD_STATE)
    joist_regs_from_mach (&j, (const struct i386_thread_state *) state);
  else
    j.tls = ((const struct i386_fsgs_base_state *) state)->fs_base;
  return __joist_sys (JOIST_SYS_THREAD_WRITE_REGS, thread, (uintptr_t) &j, 0,
		      0, NULL) == JOIST_OK ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}
weak_alias (__thread_set_state, thread_set_state)
