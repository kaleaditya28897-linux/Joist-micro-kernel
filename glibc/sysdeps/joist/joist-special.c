/* A task's special ports on Joist (Phase 5 step 7): the bootstrap port
   alone, kept by libc: the slot start-up fixed (joist-thread.h) until the
   Hurd sets another (_hurd_init does, with the one from the init ports).
   No kernel or exception ports: Joist has none (0013, 0027).
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
#include "joist-thread.h"
#include "joist-ports.h"
#include "joist_mig.h"

#if !(defined SHARED && IS_IN (libc))
mach_port_t __joist_bootstrap = JOIST_SLOT_BOOTSTRAP;
rtld_hidden_data_def (__joist_bootstrap)

/* The exit status, to the bootstrap port, when no proc server takes it
   (_exit.c): Joist's own message, JOIST_BOOT_EXIT with the status (as
   _hurd_exit has it, W_EXITCODE's) in the body's first word; the reply
   says nothing.  Phase 8's proc makes it unneeded.  */
void
__joist_report_exit (int status)
{
  if (__joist_bootstrap == MACH_PORT_NULL)
    return;
  uint64_t buf[JOIST_MIG_BUF_WORDS];
  struct joist_mig_msg m;
  m.r[0] = JOIST_BOOT_EXIT | (uint64_t) 8 << 32;
  m.r[1] = (uint32_t) status;
  m.r[2] = m.r[3] = 0;
  m.ncaps = 0;
  m.cap[0] = m.cap[1] = 0;
  m.disp[0] = m.disp[1] = 0;
  m.len = 0;
  m.buf = buf;
  m.badge = 0;
  (void) __joist_mig_call (__joist_bootstrap, &m);
}
rtld_hidden_def (__joist_report_exit)

/* The program's pid, asked of the bootstrap port at start-up when there is
   no proc server (the owner's choice, 2026-11-12: htl's robust locks put
   the owner's pid in the lock word, and 0 let every lock succeed): Joist's
   own message, JOIST_BOOT_PID, no body; the reply's first word the pid.
   0 if no starter answers.  Phase 8's proc makes it unneeded.  */
int
__joist_ask_pid (void)
{
  if (__joist_bootstrap == MACH_PORT_NULL)
    return 0;
  /* Field by field, not memset: this runs at start-up, before a static
     program's IFUNCs are resolved (only memcpy and strlen are by then).  */
  uint64_t buf[JOIST_MIG_BUF_WORDS];
  struct joist_mig_msg m;
  m.r[0] = JOIST_BOOT_PID;
  m.r[1] = m.r[2] = m.r[3] = 0;
  m.ncaps = 0;
  m.cap[0] = m.cap[1] = 0;
  m.disp[0] = m.disp[1] = 0;
  m.len = 0;
  m.buf = buf;
  m.badge = 0;
  if (__joist_mig_call (__joist_bootstrap, &m) != 0 || (uint32_t) m.r[0] != 0
      || m.r[1] == 0 || m.r[1] > INT32_MAX)
    return 0;
  return (int) m.r[1];
}
rtld_hidden_def (__joist_ask_pid)
#endif
