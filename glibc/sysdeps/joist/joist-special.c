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
  memset (&m, 0, sizeof m);
  m.buf = buf;
  m.r[0] = JOIST_BOOT_EXIT | (uint64_t) 8 << 32;
  m.r[1] = (uint32_t) status;
  (void) __joist_mig_call (__joist_bootstrap, &m);
}
rtld_hidden_def (__joist_report_exit)
#endif
