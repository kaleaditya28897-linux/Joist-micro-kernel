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

/* Joist's copy of hurd/pid2task.c (Phase 5 step 7), changed in one place:
   with no proc server, the program's own pid names its own task (htl's
   robust locks ask whether their owner's pid is alive: hurdlock.c's
   valid_pid); any other pid is refused as before.  */

#include <hurd.h>

task_t
__pid2task (pid_t pid)
{
  error_t err;
  task_t task;

  if (pid != 0 && pid == _hurd_pid)
    /* A fixed name: deallocating it changes nothing (joist-ports.h).  */
    return __mach_task_self ();

  err = __USEPORT (PROC, __proc_pid2task (port, pid, &task));

  return err ? (__hurd_fail (err), MACH_PORT_NULL) : task;
}

weak_alias (__pid2task, pid2task)
