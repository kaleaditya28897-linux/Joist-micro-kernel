/* libc's port names on Joist: the table, its lock and its kernel calls
   (Phase 5 step 7, the design's items 2 and 3).
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

/* One table in a program (joist-ports.h): nothing here in libc.so.  */
#if !(defined SHARED && IS_IN (libc))

#include "joist-sys.h"
#include "joist-names.c"

#include <libc-lock.h>

static struct jn_table table;
static int table_made;
__libc_lock_define_initialized (static, lock)

struct jn_table *
__joist_ports_lock (void)
{
  __libc_lock_lock (lock);
  if (!table_made)
    {
      __jn_init (&table, JOIST_FIXED_NAMES);
      table_made = 1;
    }
  return &table;
}
rtld_hidden_def (__joist_ports_lock)

void
__joist_ports_unlock (void)
{
  __libc_lock_unlock (lock);
}
rtld_hidden_def (__joist_ports_unlock)

rtld_hidden_def (__jn_allocate)
rtld_hidden_def (__jn_deallocate)
rtld_hidden_def (__jn_destroy)
rtld_hidden_def (__jn_get_refs)
rtld_hidden_def (__jn_insert_right)
rtld_hidden_def (__jn_mod_refs)
rtld_hidden_def (__jn_move_member)
rtld_hidden_def (__jn_reply_name)
rtld_hidden_def (__jn_set_payload)
rtld_hidden_def (__jn_type)

/* The table's environment (joist-names.h): the kernel's calls on this
   task's own space, their codes as the kernel gives them.  */

int
__joist_names_endpoint (uint32_t dst)
{
  return __joist_sys (JOIST_SYS_OBJ_CREATE, JOIST_OBJ_ENDPOINT, dst, 0, 0,
		      NULL);
}

int
__joist_names_mint (uint32_t src, uint32_t dst, uint64_t badge)
{
  return __joist_sys (JOIST_SYS_CAP_MINT, src, dst,
		      JOIST_R_SEND | JOIST_R_GRANT, badge, NULL);
}

int
__joist_names_derive (uint32_t src, uint32_t dst)
{
  return __joist_sys (JOIST_SYS_CAP_DERIVE, src, dst,
		      JOIST_R_SEND | JOIST_R_GRANT, 0, NULL);
}

int
__joist_names_delete (uint32_t slot)
{
  return __joist_sys (JOIST_SYS_CAP_DELETE, slot, 0, 0, 0, NULL);
}

int
__joist_names_grow (uint32_t leaf)
{
  return __joist_sys (JOIST_SYS_CSPACE_GROW, leaf, 0, 0, 0, NULL);
}

int
__joist_names_shrink (uint32_t leaf)
{
  return __joist_sys (JOIST_SYS_CSPACE_SHRINK, leaf, 0, 0, 0, NULL);
}

#endif
