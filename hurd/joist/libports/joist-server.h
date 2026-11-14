/* joist-server.h - what libports on Joist uses of glibc's Joist runtime
   (Phase 5 step 8; the owner's design of 2026-11-12).
   Copyright (C) 2026 Aditya Kale.

   This file is part of Joist's libports, from the GNU Hurd's.

   It is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 2, or (at your option) any later
   version.

   It is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.  */

/* glibc/sysdeps/joist/joist-mig.c's server side, which libports links
   from libc.a (its declarations are glibc's own, in joist-ports.h, which
   only glibc can include).  Each returns 0 or a Mach error.  */

#ifndef _JOIST_SERVER_H
#define _JOIST_SERVER_H 1

#include <stdint.h>
#include <mach.h>
#include <joist_mig.h>

#define JOIST_FOREVER UINT64_MAX

/* A request on the port or port set NAME, for at most TIMEOUT ns.  */
extern int __joist_mig_receive (mach_port_t name, struct joist_mig_msg *in,
				uint64_t timeout);
/* OUT answers the request received last; then the next, as above.  */
extern int __joist_mig_reply_receive (struct joist_mig_msg *out,
				      mach_port_t name,
				      struct joist_mig_msg *in,
				      uint64_t timeout);
/* OUT answers the request received last.  */
extern int __joist_mig_reply (struct joist_mig_msg *out);
/* No-senders for the port set SET: a notification bound to it (*NOTIF),
   a wait on it (0, or EINTR), the next badge that lost its last sender
   (0 if none) with its port's make-send count as the firing was taken.  */
extern int __joist_nosenders_bind (mach_port_t set, mach_port_t *notif);
extern int __joist_nosenders_wait (mach_port_t notif);
extern int __joist_nosenders_next (mach_port_t set, uint64_t *badge,
				   mach_port_mscount_t *mscount);

#endif
