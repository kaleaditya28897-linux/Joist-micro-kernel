/* MIG's reply ports on Joist (Phase 5 step 7): the reply sentinel.
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

/* A Joist call has a reply capability of its own (decision 0001), so a
   MIG stub needs no reply port.  What code outside MIG asks for, it gets:
   one reply name for the task (mach_reply_port), made once and never
   deallocated (the design's item 4).  */

#include <mach.h>
#include <mach/mig_support.h>
#include <atomic.h>
#include "joist-ports.h"

static mach_port_t sentinel;

#if !defined (SHARED) || IS_IN (rtld)
/* The Hurd's TLS set-up (tls.h) copies it into the first thread's TCB;
   unused on Joist, kept for it.  */
mach_port_t __hurd_reply_port0;
#endif

/* The reply sentinel.  */
mach_port_t
__mig_get_reply_port (void)
{
  mach_port_t port = atomic_load_acquire (&sentinel);
  if (__glibc_unlikely (port == MACH_PORT_NULL))
    {
      struct jn_table *t = __joist_ports_lock ();
      port = sentinel;
      if (port == MACH_PORT_NULL && __jn_reply_name (t, &port) == JN_OK)
	atomic_store_release (&sentinel, port);
      __joist_ports_unlock ();
    }
  return port;
}
weak_alias (__mig_get_reply_port, mig_get_reply_port)
libc_hidden_def (__mig_get_reply_port)

/* The sentinel stays: nothing to deallocate.  */
void
__mig_dealloc_reply_port (mach_port_t port)
{
  (void) port;
}
weak_alias (__mig_dealloc_reply_port, mig_dealloc_reply_port)
libc_hidden_def (__mig_dealloc_reply_port)

void
__mig_put_reply_port (mach_port_t port)
{
  (void) port;
}
weak_alias (__mig_put_reply_port, mig_put_reply_port)

void
__mig_init (void *stack)
{
  (void) stack;
}
weak_alias (__mig_init, mig_init)
libc_hidden_def (__mig_init)
