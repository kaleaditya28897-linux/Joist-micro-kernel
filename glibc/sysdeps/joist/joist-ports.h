/* libc's port names on Joist: the table and its lock (Phase 5 step 7).
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

/* The task's one name table (joist-names.h), under one lock: the
   mach_port_* calls take it for the whole call, kernel calls included
   (none of them blocks).  Names below JOIST_FIXED_NAMES are the slots
   start-up fills (the design's item 8).  Only the task's own names are
   here: a call naming another task is refused with
   MACH_SEND_INVALID_DEST, as for a task port that names nothing (another
   task's names are Phase 7's, with fork).  */

#ifndef _JOIST_PORTS_H
#define _JOIST_PORTS_H 1

#include <stddef.h>
#include <mach.h>
#include "joist-names.h"

#define JOIST_FIXED_NAMES 16u

/* The table, locked (made on first use).  A program has one table: in
   ld.so if it is dynamic (ld.so runs first; libc.so and libmachuser use
   its table through these and the __jn_* calls, exported at
   GLIBC_PRIVATE: Versions), in libc.a if it is static.  */
extern struct jn_table *__joist_ports_lock (void);
extern void __joist_ports_unlock (void);
rtld_hidden_proto (__joist_ports_lock)
rtld_hidden_proto (__joist_ports_unlock)
rtld_hidden_proto (__jn_allocate)
rtld_hidden_proto (__jn_deallocate)
rtld_hidden_proto (__jn_destroy)
rtld_hidden_proto (__jn_get_refs)
rtld_hidden_proto (__jn_insert_right)
rtld_hidden_proto (__jn_mod_refs)
rtld_hidden_proto (__jn_move_member)
rtld_hidden_proto (__jn_reply_name)
rtld_hidden_proto (__jn_set_payload)
rtld_hidden_proto (__jn_type)
rtld_hidden_proto (__jn_send_prepare)
rtld_hidden_proto (__jn_send_done)
rtld_hidden_proto (__jn_landing)
rtld_hidden_proto (__jn_arrived)
rtld_hidden_proto (__jn_unland)
rtld_hidden_proto (__jn_pinned)
rtld_hidden_proto (__jn_unpin)
/* MIG's runtime (joist-mig.c, joist_mig.h): one in a program, as the
   table.  */
struct joist_mig_msg;
extern int __joist_mig_call (uint32_t port, struct joist_mig_msg *m);
extern void *__joist_mig_allocate (size_t size);
extern void __joist_mig_deallocate (void *p, size_t size);
rtld_hidden_proto (__joist_mig_call)
/* Threads' notifications and the gsync calls (joist-sync.c, joist-thread.h):
   one in a program, as the table.  */
struct joist_thread;
extern struct joist_thread __joist_thread0;
extern int __joist_thread_make (struct joist_thread *t, uint32_t self);
extern void __joist_thread_free (struct joist_thread *t);
extern int __joist_thread_wait (struct joist_thread *t, uint64_t bit,
				uint64_t timeout);
extern void __joist_thread_signal (const struct joist_thread *t,
				   uint64_t bit);
extern int __joist_sync_init (void);
/* Scheduling contexts terminated threads left, for the next (joist-sync.c).  */
extern uint32_t __joist_sc_reuse (void);
extern void __joist_sc_keep (uint32_t sc);
/* thread_terminate's two halves (RPC_thread_terminate.c): the name
   forgotten, then the kernel's calls alone.  */
extern kern_return_t __joist_thread_forget (thread_t thread);
extern kern_return_t __joist_thread_end (thread_t thread, int self);
extern kern_return_t __joist_gsync_wait_call (mach_port_t task,
					      vm_offset_t addr,
					      unsigned int val1,
					      unsigned int val2,
					      natural_t msec, int flags);
extern kern_return_t __joist_gsync_wake_call (mach_port_t task,
					      vm_offset_t addr,
					      unsigned int val, int flags);
extern kern_return_t __joist_gsync_requeue_call (mach_port_t task,
						 vm_offset_t src,
						 vm_offset_t dst,
						 boolean_t wake_one,
						 int flags);
rtld_hidden_proto (__joist_thread0)
rtld_hidden_proto (__joist_thread_make)
rtld_hidden_proto (__joist_thread_free)
rtld_hidden_proto (__joist_thread_wait)
rtld_hidden_proto (__joist_thread_signal)
rtld_hidden_proto (__joist_sync_init)
rtld_hidden_proto (__joist_sc_reuse)
/* The bootstrap port (joist-special.c).  */
extern mach_port_t __joist_bootstrap;
rtld_hidden_proto (__joist_bootstrap)
/* The exit status to the bootstrap port (joist-special.c), when there is
   no proc server: the message's id; and the call.  */
#define JOIST_BOOT_EXIT 0x4a4f0001u
extern void __joist_report_exit (int status);
rtld_hidden_proto (__joist_report_exit)
rtld_hidden_proto (__joist_sc_keep)
rtld_hidden_proto (__joist_thread_forget)
rtld_hidden_proto (__joist_thread_end)
rtld_hidden_proto (__joist_gsync_wait_call)
rtld_hidden_proto (__joist_gsync_wake_call)
rtld_hidden_proto (__joist_gsync_requeue_call)
rtld_hidden_proto (__joist_mig_allocate)
rtld_hidden_proto (__joist_mig_deallocate)

/* Whether TASK is this task.  */
#define JOIST_PORTS_SELF(task) ((task) == __mach_task_self ())

/* The body of a mach_port_* call on TASK: CALL, with T the table.  */
#define JOIST_PORTS_CALL(task, call)				\
  ({								\
    kern_return_t __r = MACH_SEND_INVALID_DEST;			\
    if (JOIST_PORTS_SELF (task))				\
      {								\
	struct jn_table *t = __joist_ports_lock ();		\
	__r = (call);						\
	__joist_ports_unlock ();				\
      }								\
    __r;							\
  })

#endif
