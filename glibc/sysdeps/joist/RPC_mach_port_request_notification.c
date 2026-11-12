/* mach_port_request_notification on Joist (Phase 5 step 8, libports).
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

/* A no-senders request for a receive right is accepted: Joist counts a
   port's senders by its badge and queues its firing on its set's endpoint
   (I4), where libports' notifier collects it (the owner's design of
   2026-11-12); no message goes to NOTIFY, so *PREVIOUS is always null.
   The rest (dead names, port destroyed, ...) are refused: a client's death
   is its reply's E_PEER on Joist, and Mach's other notifications have no
   counterpart yet.  */

#include <mach.h>
#include <mach/notify.h>
#include "joist-ports.h"

kern_return_t
__mach_port_request_notification (mach_port_t task, mach_port_name_t name,
				  mach_msg_id_t id, mach_port_mscount_t sync,
				  mach_port_t notify,
				  mach_msg_type_name_t notifyPoly,
				  mach_port_t *previous)
{
  (void) sync;
  (void) notify;
  (void) notifyPoly;
  if (task != __mach_task_self ())
    return MACH_SEND_INVALID_DEST;	/* another task's names: Phase 7 */
  if (id != MACH_NOTIFY_NO_SENDERS)
    return KERN_INVALID_VALUE;
  struct jn_table *t = __joist_ports_lock ();
  uint32_t type;
  int r = __jn_type (t, name, &type);
  __joist_ports_unlock ();
  if (r != JN_OK)
    return r;
  if (!(type & JN_TYPE_RECEIVE))
    return KERN_INVALID_RIGHT;
  *previous = MACH_PORT_NULL;
  return KERN_SUCCESS;
}
weak_alias (__mach_port_request_notification, mach_port_request_notification)
