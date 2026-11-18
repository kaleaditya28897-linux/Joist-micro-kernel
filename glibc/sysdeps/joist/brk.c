/* Copyright (C) 1991-2026 Free Software Foundation, Inc.
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


/* Joist's copy (Phase 5 step 11; the owner's choice of 2026-11-18): the
   break, and with it malloc's main arena (MORECORE is sbrk), in one region
   from mm (joist-mig.c's __joist_brk_hold and __joist_brk_grow): its 8
   MiB range held from start-up, the region made at the first growth and
   grown by doubling extents from 64 KiB, pages mapped by the task as the
   break goes up, kept mapped when it comes down. Past the range, or if mm refuses, ENOMEM: malloc then goes on with
   mmap, as it does whenever sbrk fails. The Hurd's version reserves 128
   MiB after the program's end with vm_map and opens it with vm_protect;
   a region's range must lie on page-table boundaries no mapping shares,
   so the break starts there instead. RLIMIT_DATA isn't checked: no proc
   server sets it in Phase 5.  */

#include <errno.h>
#include <hurd.h>
#include <lock-intern.h>	/* For `struct mutex'.  */

#include "set-hooks.h"
#include "joist-ports.h"

/* Up to the page including this address is mapped.  */
vm_address_t _hurd_data_end;

/* Up to this address is actually available to the user.  */
vm_address_t _hurd_brk = 0;

/* This name is used by the Linux crtbeginS.o for reasons you don't even
   want to think about it.  It's just easier to provide some definition for
   it than even to explain the braindamage involved.  */
weak_alias (_hurd_brk, ___brk_addr)

struct mutex _hurd_brk_lock;

static vm_address_t brk_start, brk_end;


/* Set the end of the process's data space to INADDR.
   Return 0 if successful, -1 if not.  */
int
__brk (void *inaddr)
{
  int ret;
  HURD_CRITICAL_BEGIN;
  __mutex_lock (&_hurd_brk_lock);
  ret = _hurd_set_brk ((vm_address_t) inaddr);
  __mutex_unlock (&_hurd_brk_lock);
  HURD_CRITICAL_END;
  return ret;
}
weak_alias (__brk, brk)


int
_hurd_set_brk (vm_address_t addr)
{
  if (brk_start == 0 || addr < brk_start || addr > brk_end)
    return __hurd_fail (ENOMEM);
  if (addr > _hurd_data_end)
    {
      uint64_t mapped;
      if (__joist_brk_grow (addr, &mapped) != 0)
	return __hurd_fail (ENOMEM);
      _hurd_data_end = mapped;
    }
  _hurd_brk = addr;
  return 0;
}

static void attribute_used_retain
init_brk (void)
{
  uint64_t lo, hi;

  __mutex_init (&_hurd_brk_lock);

  if (__joist_brk_hold (&lo, &hi) == 0)
    {
      brk_start = lo;
      brk_end = hi;
      _hurd_brk = lo;
      _hurd_data_end = lo;
    }
}
SET_RELHOOK (_hurd_preinit_hook, init_brk);
