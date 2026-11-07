/* Initial program startup for running under the GNU Hurd.  On Joist.
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

/* Joist replaces hurd/hurdstartup.c (Phase 5 step 7, the design's item 8).
   There is no exec server to ask: the root (Joist's own code, which plays
   it) laid the new task's stack out as the Hurd's would after its
   exec_startup_get_info: ARGPTR is argc, then argv and envp (each ending
   in a null pointer), then a struct hurd_startup_data, all on the stack,
   and the arrays it points to (the descriptor table, the init ports and
   the init ints) in pages of their own, which glibc may deallocate.  The
   names in them are the task's slots.  */

#include <stdint.h>
#include <hurd.h>
#include "hurdstartup.h"

mach_port_t *_hurd_init_dtable;
mach_msg_type_number_t _hurd_init_dtablesize;

extern void __mach_init (void);

void
_hurd_startup (void **argptr, void (*main) (intptr_t *data, void *ctx),
	       void *ctx)
{
  __mach_init ();

  intptr_t *argcptr = (intptr_t *) argptr;
  char **argv = (char **) &argcptr[1];
  char **envp = &argv[argcptr[0] + 1];
  int envc = 0;
  while (envp[envc] != NULL)
    envc++;
  struct hurd_startup_data *d = (void *) &envp[envc + 1];
  _hurd_init_dtable = d->dtable;
  _hurd_init_dtablesize = d->dtablesize;

  (*main) (argcptr, ctx);

  /* Not reached: MAIN doesn't return.  */
  __builtin_trap ();
}
