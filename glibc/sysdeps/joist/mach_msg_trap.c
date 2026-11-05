/* mach_msg_trap on Joist (Phase 5 step 7): not provided (the design's
   items 4 and 10), as mach_msg (msg.c).
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

mach_msg_return_t
__mach_msg_trap (mach_msg_header_t *msg, mach_msg_option_t option,
		 mach_msg_size_t send_size, mach_msg_size_t rcv_size,
		 mach_port_t rcv_name, mach_msg_timeout_t timeout,
		 mach_port_t notify)
{
  return MACH_SEND_INVALID_HEADER;
}
weak_alias (__mach_msg_trap, mach_msg_trap)
libc_hidden_def (__mach_msg_trap)
