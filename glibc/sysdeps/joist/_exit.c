/* Copyright (C) 1993-2026 Free Software Foundation, Inc.
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

/* Joist's copy of sysdeps/mach/hurd/_exit.c (Phase 5 step 7, the design's
   item 11 d), changed in one place: with no proc server to give the exit
   status to, it goes to the bootstrap port, if there is one (the program's
   starter: user/glibcroot), in a message of Joist's own (joist-special.c),
   before the task ends itself.  */

#include <unistd.h>
#include <hurd.h>
#include <hurd/port.h>
#include <sysdep.h>
#include <sys/wait.h>
#include "joist-ports.h"

void
_hurd_exit (int status)
{
  if (_hurd_ports != NULL)
    /* Give the proc server our exit status.  */
    __USEPORT (PROC, __proc_mark_exit (port, status, 0));
  /* Joist: none?  Then the program's starter.  */
  if (_hurd_ports == NULL || _hurd_ports[INIT_PORT_PROC].port == MACH_PORT_NULL)
    __joist_report_exit (status);

  /* Commit suicide.  */
  __task_terminate (__mach_task_self ());

  /* Perhaps the cached mach_task_self was bogus.  */
  __task_terminate ((__mach_task_self) ());

  /* This sucker really doesn't want to die.  */
  while (1)
    {
#ifdef LOSE
      LOSE;
#else
      volatile const int zero = 0, one = 1;
      volatile int lossage = one / zero;
#endif
    }
}

void
_exit (int status)
{
  _hurd_exit (W_EXITCODE (status, 0));
}
libc_hidden_def (_exit)
#ifndef NO_RTLD_HIDDEN
rtld_hidden_def (_exit)
#endif
weak_alias (_exit, _Exit)
