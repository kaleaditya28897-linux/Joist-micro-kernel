/* vm_statistics on Joist (Phase 5 step 7): the page size.
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

/* glibc asks for it once, at start-up (__mach_init), for the page size.
   Joist's pages are 4 KiB.  The counts are mm's, which libc doesn't
   know: zero.  */

#include <string.h>
#include <mach.h>
#include <mach/vm_statistics.h>

kern_return_t
__vm_statistics (task_t task, vm_statistics_data_t *stats)
{
  if (task != __mach_task_self ())
    return KERN_INVALID_ARGUMENT;
  memset (stats, 0, sizeof *stats);
  stats->pagesize = 4096;
  return KERN_SUCCESS;
}
weak_alias (__vm_statistics, vm_statistics)
