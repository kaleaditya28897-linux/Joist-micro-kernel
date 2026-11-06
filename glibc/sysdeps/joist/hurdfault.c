/* Faults in the signal thread, on Joist.
   Copyright (C) 1994-2026 Free Software Foundation, Inc.
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

/* Joist replaces hurd/hurdfault.c (Phase 5 step 7).  The Hurd catches a
   fault in the signal thread by having the proc server forward its
   exception to a port of the signal thread's, then running the message
   through the faultexc demuxer as Mach's (mach_msg_header_t) by hand; on
   Joist that demuxer is joistmig's, faults go to mm or a fault handler
   (decision 0027), and there is no proc server yet.  Phase 5 has no
   signal thread (the design's item 8), so nothing is caught here:
   _hurdsig_fault_init does nothing.  What the rest of glibc uses (the
   preemptor and the jump buffer of _hurdsig_catch_fault, and the faultexc
   routines the generated demuxer calls) is kept, as glibc exports it.  */

#include <hurd.h>
#include <hurd/signal.h>
#include "hurdfault.h"
#include <setjmp.h>
#include "faultexc_server.h"	/* mig-generated header for our exc server.  */

jmp_buf _hurdsig_fault_env;
struct hurd_signal_preemptor _hurdsig_fault_preemptor = {0};

/* XXX temporary to deal with spelling fix */
weak_alias (_hurdsig_fault_preemptor, _hurdsig_fault_preempter)

/* No exception is forwarded to this task (above): none is expected.  */
kern_return_t
_hurdsig_fault_catch_exception_raise (mach_port_t port,
				      thread_t thread,
				      task_t task,
#ifdef EXC_MASK_ALL		/* New interface flavor.  */
				      exception_type_t exception,
				      exception_data_t code,
				      mach_msg_type_number_t codeCnt
#else				/* Vanilla Mach 3.0 interface.  */
				      integer_t exception,
				      integer_t code, long_integer_t subcode
#endif
				      )
{
  return EPERM;
}

#ifdef EXC_MASK_ALL
kern_return_t
_hurdsig_fault_catch_exception_raise_state
(mach_port_t port,
 exception_type_t exception,
 exception_data_t code,
 mach_msg_type_number_t codeCnt,
 int *flavor,
 thread_state_t old_state,
 mach_msg_type_number_t old_stateCnt,
 thread_state_t new_state,
 mach_msg_type_number_t *new_stateCnt)
{
  return KERN_FAILURE;
}

kern_return_t
_hurdsig_fault_catch_exception_raise_state_identity
(mach_port_t exception_port,
 thread_t thread,
 task_t task,
 exception_type_t exception,
 exception_data_t code,
 mach_msg_type_number_t codeCnt,
 int *flavor,
 thread_state_t old_state,
 mach_msg_type_number_t old_stateCnt,
 thread_state_t new_state,
 mach_msg_type_number_t *new_stateCnt)
{
  return KERN_FAILURE;
}
#endif

/* Signals are Phase 7's: no exception port to set up.  */
void
_hurdsig_fault_init (void)
{
}
