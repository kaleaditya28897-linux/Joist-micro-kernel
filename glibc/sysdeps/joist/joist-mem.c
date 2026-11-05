/* The task's address reservations in libc (Phase 5 step 7).
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

#include "joist-mem.h"

/* One table in a program (joist-mem.h): nothing here in libc.so.  */
#if !(defined SHARED && IS_IN (libc))

#include "joist-sys.h"
#include "joist-vm.c"

#include <libc-lock.h>

static struct jv_table table;
__libc_lock_define_initialized (static, lock)

/* The table starts empty (bss): what start-up mapped (the program, its
   stack) is not in it, so vm_allocate can't place anything there.  */
struct jv_table *
__joist_vm_lock (void)
{
  __libc_lock_lock (lock);
  return &table;
}
rtld_hidden_def (__joist_vm_lock)

void
__joist_vm_unlock (void)
{
  __libc_lock_unlock (lock);
}
rtld_hidden_def (__joist_vm_unlock)

rtld_hidden_def (__jv_allocate)
rtld_hidden_def (__jv_deallocate)
rtld_hidden_def (__jv_vm_protect)
rtld_hidden_def (__jv_vm_region)

/* The table's environment (joist-vm.h): a request to mm, its result; a
   Call the kernel refused is none of mm's results (KERN_FAILURE).  */
int
__joist_vm_mm (int req, uint64_t page, uint64_t pages, uint32_t perms)
{
  static const uint32_t reqs[] =
    {
      [JV_MM_MAP_ANON] = JOIST_MM_MAP_ANON,
      [JV_MM_UNMAP] = JOIST_MM_UNMAP,
      [JV_MM_PROTECT] = JOIST_MM_PROTECT,
    };
  uint64_t msg[4] = { reqs[req], page, pages, perms };
  if (__joist_call (JOIST_MM_SLOT, msg, JOIST_FOREVER) != JOIST_OK)
    return -1;
  return msg[0] > INT32_MAX ? -1 : (int) msg[0];
}

#endif
