/* The signal thread's message port, on Joist.
   Copyright (C) 1991-2026 Free Software Foundation, Inc.
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

/* Joist replaces hurd/msgportdemux.c (Phase 5 step 7).  The Hurd's loop
   receives with mach_msg and calls the msg and exc demuxers as Mach's
   (mach_msg_header_t); on Joist both are joistmig's, which take Joist
   messages, and mach_msg isn't provided (the design's item 4).  Signal
   delivery is Phase 7 (item 10), when this loop becomes a ReplyRecv loop
   on the demuxers; until then the signal thread isn't started (item 8),
   and if it were, it stops the task here rather than call a demuxer with
   the wrong type.  */

#include <hurd.h>
#include <hurd/signal.h>
#include <stdio.h>	/* __libc_fatal */

void *
_hurd_msgport_receive (void *arg)
{
  (void) arg;
  __libc_fatal ("Joist: the signal thread's message port comes with "
		"signals (Phase 7)\n");
}
