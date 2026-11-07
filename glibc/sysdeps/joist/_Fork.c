/* fork refused, Joist (Phase 5 step 7, the design's item 10).
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

/* In place of sysdeps/mach/hurd/_Fork.c, which needs the proc server
   (proc_dostop, proc_task2proc), another task's memory and threads
   (task_create, task_threads, thread_get_state), and the signal thread:
   all Phase 7.  Without them it took its locks (the descriptor table's,
   the signal state's, malloc's), then failed an assert_perror on
   proc_dostop's error, whose message needs those locks: the program
   spun on them for good.  This fails before taking any: fork (and vfork,
   the Hurd's alias of it) returns -1 with ENOSYS, and its caller,
   posix/fork.c, unlocks what it locked, as for any failed _Fork.  */

#include <errno.h>
#include <unistd.h>
#include <hurd.h>

pid_t
_Fork (void)
{
  return __hurd_fail (ENOSYS);
}
libc_hidden_def (_Fork)
