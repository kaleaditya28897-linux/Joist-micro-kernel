/* host_info on Joist (Phase 5 step 7, the design's item 9): HOST_BASIC_INFO
   on the host's name, the CPU count from the kernel's clock call; the
   memory size isn't known to a task (mm keeps it: 0).  No other flavor.
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
#include <mach/host_info.h>
#include <mach/machine.h>
#include <mach/mig_errors.h>
#include "joist-sys.h"
#include "joist-thread.h"

kern_return_t
__host_info (host_t host, int flavor, host_info_t info,
	     mach_msg_type_number_t *count)
{
  if (host != JOIST_SLOT_HOST)
    return KERN_INVALID_ARGUMENT;
  if (flavor != HOST_BASIC_INFO)
    return KERN_INVALID_ARGUMENT;
  if (*count < HOST_BASIC_INFO_COUNT)
    return MIG_ARRAY_TOO_LARGE;
  uint64_t ns, cpus;
  __joist_clock (&ns, &cpus);
  struct host_basic_info *b = (struct host_basic_info *) info;
  b->max_cpus = b->avail_cpus = (integer_t) cpus;
  b->memory_size = 0;
  b->cpu_type = CPU_TYPE_X86_64;
  b->cpu_subtype = 0;
  *count = HOST_BASIC_INFO_COUNT;
  return KERN_SUCCESS;
}
weak_alias (__host_info, host_info)
