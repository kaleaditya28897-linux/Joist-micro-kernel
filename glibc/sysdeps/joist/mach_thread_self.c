/* mach_thread_self on Joist (Phase 5 step 7): the calling thread's own
   capability, from its record (joist-thread.h).  A pinned name: the
   deallocate a caller makes after it changes nothing.
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
#include <tls.h>

mach_port_t
__mach_thread_self (void)
{
  return __joist_thread_self ()->self;
}
weak_alias (__mach_thread_self, mach_thread_self)
libc_hidden_def (__mach_thread_self)
