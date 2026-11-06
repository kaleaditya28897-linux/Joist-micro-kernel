/* thread_get_state on Joist (Phase 5 step 7): i386_THREAD_STATE and
   i386_FSGS_BASE_STATE from the thread's register record; no other
   flavor (the floating point state is Phase 7's, with signals).
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
#include <mach/mig_errors.h>

kern_return_t
__thread_get_state (thread_t thread, int flavor, thread_state_t state,
		    mach_msg_type_number_t *count)
{
  struct joist_regs j;
  if (flavor == i386_THREAD_STATE)
    {
      if (*count < i386_THREAD_STATE_COUNT)
	return MIG_ARRAY_TOO_LARGE;
      kern_return_t r = joist_regs_read (thread, &j);
      if (r != KERN_SUCCESS)
	return r;
      joist_regs_to_mach (&j, (struct i386_thread_state *) state);
      *count = i386_THREAD_STATE_COUNT;
      return KERN_SUCCESS;
    }
  if (flavor == i386_FSGS_BASE_STATE)
    {
      if (*count < i386_FSGS_BASE_STATE_COUNT)
	return MIG_ARRAY_TOO_LARGE;
      kern_return_t r = joist_regs_read (thread, &j);
      if (r != KERN_SUCCESS)
	return r;
      struct i386_fsgs_base_state *s = (struct i386_fsgs_base_state *) state;
      s->fs_base = j.tls;
      s->gs_base = 0;
      *count = i386_FSGS_BASE_STATE_COUNT;
      return KERN_SUCCESS;
    }
  return KERN_INVALID_ARGUMENT;
}
weak_alias (__thread_get_state, thread_get_state)
