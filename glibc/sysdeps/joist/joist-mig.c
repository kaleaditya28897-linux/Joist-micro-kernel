/* The runtime of MIG's Joist mode in glibc: joist_mig_call and the memory
   for out-of-line data (Phase 5 step 7, the design's item 4).
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

/* An RPC is one Call (decision 0001) on the slot the port's name is, with
   no timeout.  Its rights are made from the name table first (a send
   right from a receive right is minted into a slot of its own, deleted
   after; a move gives up one user reference once sent); rights that come
   back land in two names kept for them, and become send rights there.
   The table's lock is not held across the Call.  */

#include "joist-ports.h"
#include "joist-sys.h"
#include <joist_mig.h>
#include <mach.h>
#include <mach/mig_errors.h>

/* One in a program, as the name table (joist-ports.h).  */
#if !(defined SHARED && IS_IN (libc))

#define JOIST_SYS_RECV_SLOTS	16
#define JOIST_SYS_IPC_BUFFER	72
#define JOIST_SLOT_NONE16	0xffffu
#define JOIST_E_GONE		10
#define JOIST_E_PEER		11
#define JOIST_E_ABORTED		14

/* The thread's IPC buffer.  For now the program's one, registered on
   first use by the thread that makes it: threads (htl) come with their
   own, kept in the TCB (the design's item 7).  */
static uint64_t *ipc_page;

static uint64_t *
ipc_buffer (void)
{
  if (__glibc_likely (ipc_page != NULL))
    return ipc_page;
  vm_address_t a = 0;
  if (__vm_allocate (__mach_task_self (), &a, JOIST_MIG_BUF_WORDS * 8, 1)
      != KERN_SUCCESS)
    return NULL;
  if (__joist_sys (JOIST_SYS_IPC_BUFFER, a, 0, 0, 0, NULL) != JOIST_OK)
    {
      __vm_deallocate (__mach_task_self (), a, JOIST_MIG_BUF_WORDS * 8);
      return NULL;
    }
  ipc_page = (uint64_t *) a;
  return ipc_page;
}

/* Call on SLOT: registers R and the capabilities and word count in *R9
   go out, the reply comes back in them.  */
static uint64_t
joist_call (uint32_t slot, uint64_t r[4], uint64_t *r9)
{
  register uint64_t rr9 __asm__ ("r9") = *r9;
  register uint64_t r12 __asm__ ("r12") = r[0];
  register uint64_t r13 __asm__ ("r13") = r[1];
  register uint64_t r14 __asm__ ("r14") = r[2];
  register uint64_t r15 __asm__ ("r15") = r[3];
  register uint64_t r10 __asm__ ("r10") = 0;
  register uint64_t r8 __asm__ ("r8") = 0;
  uint64_t rax = JOIST_SYS_CALL, rdi = slot, rsi = JOIST_FOREVER, rdx = 0;
  __asm__ volatile ("syscall"
		    : "+a" (rax), "+D" (rdi), "+S" (rsi), "+d" (rdx),
		      "+r" (r10), "+r" (r8), "+r" (rr9), "+r" (r12),
		      "+r" (r13), "+r" (r14), "+r" (r15)
		    :
		    : "rcx", "r11", "memory");
  r[0] = r12;
  r[1] = r13;
  r[2] = r14;
  r[3] = r15;
  *r9 = rr9;
  return rax;
}

/* The rights a disposition passes: a send-once right can't be passed on
   (it has no GRANT).  */
static uint64_t
mask_of (int disp)
{
  return disp == MACH_MSG_TYPE_MOVE_SEND_ONCE
	 || disp == MACH_MSG_TYPE_MAKE_SEND_ONCE
	 ? JOIST_R_SEND : JOIST_R_SEND | JOIST_R_GRANT;
}

static int
mach_error_of (uint64_t e)
{
  switch (e)
    {
    case JOIST_OK:
      return KERN_SUCCESS;
    case JOIST_E_PEER:
      return MIG_SERVER_DIED;	/* the server went before it answered */
    case JOIST_E_ABORTED:
      return MACH_SEND_INTERRUPTED;
    case JOIST_E_NOMEM:
      return MACH_SEND_NO_BUFFER;
    default:			/* the port: not one, or dead (E_GONE) */
      return MACH_SEND_INVALID_DEST;
    }
}

int
__joist_mig_call (uint32_t port, struct joist_mig_msg *m)
{
  uint32_t slot[JOIST_MIG_CAPS], land[JOIST_MIG_CAPS];
  int temp[JOIST_MIG_CAPS];
  uint32_t n = 0, nland = 0;
  int err = KERN_SUCCESS;

  if (m->ncaps > JOIST_MIG_CAPS || m->len > JOIST_MIG_BUF_WORDS)
    return MACH_SEND_MSG_TOO_LARGE;
  uint64_t *buf = ipc_buffer ();
  if (buf == NULL)
    return MACH_SEND_NO_BUFFER;

  struct jn_table *t = __joist_ports_lock ();
  uint32_t type;
  if (__jn_type (t, port, &type) != JN_OK || !(type & JN_TYPE_SEND))
    err = MACH_SEND_INVALID_DEST;
  for (; err == KERN_SUCCESS && n < m->ncaps; n++)
    if (__jn_send_prepare (t, m->cap[n], m->disp[n], &slot[n], &temp[n])
	!= JN_OK)
      err = MACH_SEND_INVALID_RIGHT;
  if (err == KERN_SUCCESS)
    n = m->ncaps;
  else if (n > 0)
    n--;			/* the one that failed made nothing */
  while (err == KERN_SUCCESS && nland < JOIST_MIG_CAPS)
    {
      if (__jn_landing (t, &land[nland]) != JN_OK)
	err = MACH_RCV_TOO_LARGE;	/* no name left for a right to land in */
      else
	nland++;
    }
  __joist_ports_unlock ();

  uint64_t r9 = 0, e = JOIST_E_GONE;
  if (err == KERN_SUCCESS)
    {
      (void) __joist_sys (JOIST_SYS_RECV_SLOTS, land[0], land[1],
			  JOIST_MIG_CAPS, NULL);
      r9 = n;
      for (uint32_t i = 0; i < n; i++)
	r9 |= (uint64_t) slot[i] << (2 + 20 * i)
	      | mask_of (m->disp[i]) << (18 + 20 * i);
      r9 |= (uint64_t) m->len << 48;
      if (m->len > 0)
	memcpy (buf, m->buf, (size_t) m->len * 8);
      e = joist_call (port, m->r, &r9);
      err = mach_error_of (e);
    }

  t = __joist_ports_lock ();
  for (uint32_t i = 0; i < n; i++)
    __jn_send_done (t, m->cap[i], m->disp[i], slot[i], temp[i],
		    e == JOIST_OK);
  uint32_t got = e == JOIST_OK ? (uint32_t) (r9 & 3) : 0;
  for (uint32_t i = 0; i < nland; i++)
    {
      uint32_t s = i < got ? (uint32_t) ((r9 >> (2 + 20 * i)) & 0xffff)
			   : JOIST_SLOT_NONE16;
      if (s != JOIST_SLOT_NONE16 && s == land[i])
	{
	  __jn_arrived (t, s);
	  m->cap[i] = s;
	}
      else
	{
	  __jn_unland (t, land[i]);
	  if (i < got)
	    m->cap[i] = MACH_PORT_NULL;
	}
    }
  __joist_ports_unlock ();

  if (err != KERN_SUCCESS)
    return err;
  m->ncaps = got;
  m->len = (uint32_t) ((r9 >> 48) & 0x3ff);
  if (m->len > JOIST_MIG_BUF_WORDS)
    m->len = 0;
  if (m->len > 0)
    memcpy (m->buf, buf, (size_t) m->len * 8);
  return KERN_SUCCESS;
}
rtld_hidden_def (__joist_mig_call)

void *
__joist_mig_allocate (size_t size)
{
  vm_address_t a = 0;
  if (__vm_allocate (__mach_task_self (), &a, size != 0 ? size : 1, 1)
      != KERN_SUCCESS)
    return NULL;
  return (void *) a;
}
rtld_hidden_def (__joist_mig_allocate)

void
__joist_mig_deallocate (void *p, size_t size)
{
  if (p != NULL)
    __vm_deallocate (__mach_task_self (), (vm_address_t) p,
		     size != 0 ? size : 1);
}
rtld_hidden_def (__joist_mig_deallocate)

#endif
