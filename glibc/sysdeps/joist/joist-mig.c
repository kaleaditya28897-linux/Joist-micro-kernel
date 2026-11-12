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
#include "joist-thread.h"
#include <joist_mig.h>
#include <mach.h>
#include <mach/mig_errors.h>
#include <tls.h>
#include <libc-lock.h>
#include <errno.h>

/* One in a program, as the name table (joist-ports.h).  */
#if !(defined SHARED && IS_IN (libc))

#define JOIST_SYS_RECV_SLOTS	16
#define JOIST_SYS_IPC_BUFFER	72
#define JOIST_SLOT_NONE16	0xffffu
#define JOIST_E_TIMEOUT		9
#define JOIST_E_GONE		10
#define JOIST_E_PEER		11
#define JOIST_E_ABORTED		14

/* A thread's IPC buffer, in its record (joist-thread.h, the design's item
   7): made by pthread_create (pt-setup.c), so that a thread that runs has
   one whatever memory is left by then; the first thread's on its first
   call.  Only a thread can register its own: it does on its first call.
   Given back when the thread ends (pt-thread-terminate.c).  The pages
   come IPC_CHUNK at a time, one mm region for that many threads (a task
   has few regions), and a page given back is kept for the next thread,
   on a list through the pages' first words: never returned, so the most
   threads alive at once bound them, as the stacks htl keeps.  */
#define IPC_CHUNK 16u
#define IPC_PAGE (JOIST_MIG_BUF_WORDS * 8)
static uint64_t *ipc_free;
__libc_lock_define_initialized (static, ipc_lock)

int
__joist_ipc_make (struct joist_thread *t)
{
  __libc_lock_lock (ipc_lock);
  if (ipc_free == NULL)
    {
      vm_address_t a = 0;
      if (__vm_allocate (__mach_task_self (), &a, IPC_CHUNK * IPC_PAGE, 1)
	  != KERN_SUCCESS)
	{
	  __libc_lock_unlock (ipc_lock);
	  return KERN_RESOURCE_SHORTAGE;
	}
      for (unsigned int i = IPC_CHUNK; i-- > 0;)
	{
	  uint64_t *p = (uint64_t *) (a + i * IPC_PAGE);
	  *p = (uint64_t) (uintptr_t) ipc_free;
	  ipc_free = p;
	}
    }
  uint64_t *p = ipc_free;
  ipc_free = (uint64_t *) (uintptr_t) *p;
  __libc_lock_unlock (ipc_lock);
  t->ipc_buf = p;
  t->ipc_on = 0;
  return 0;
}
rtld_hidden_def (__joist_ipc_make)

void
__joist_ipc_free (struct joist_thread *t)
{
  uint64_t *p = t->ipc_buf;
  t->ipc_buf = NULL;
  t->ipc_on = 0;
  if (p == NULL)
    return;
  __libc_lock_lock (ipc_lock);
  *p = (uint64_t) (uintptr_t) ipc_free;
  ipc_free = p;
  __libc_lock_unlock (ipc_lock);
}
rtld_hidden_def (__joist_ipc_free)

static uint64_t *
ipc_buffer (void)
{
  struct joist_thread *self = __joist_thread_self ();
  if (__glibc_likely (self->ipc_on))
    return self->ipc_buf;
  if (self->ipc_buf == NULL && __joist_ipc_make (self) != 0)
    return NULL;
  if (__joist_sys (JOIST_SYS_IPC_BUFFER, (uintptr_t) self->ipc_buf, 0, 0, 0,
		   NULL) != JOIST_OK)
    return NULL;
  self->ipc_on = 1;
  return self->ipc_buf;
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
    return MACH_SEND_INVALID_HEADER;	/* more than a message holds */
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
			  JOIST_MIG_CAPS, 0, NULL);
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

/* ---- the server's side (libports on Joist, Phase 5 step 8) ----

   A request received on a port or port set (its endpoint: the names
   table's), its rights landing in names as a call's reply's do; an answer
   whose rights are made from names as a call's are.  Joist gives every
   received call a reply capability of its own (the design's item 4): the
   answer goes back on it, and a caller gone since makes the answer go
   nowhere (Joist's E_PEER), which is no error for a server.  */

#define JOIST_SYS_RECV		11
#define JOIST_SYS_REPLY		12
#define JOIST_SYS_REPLY_RECV	13

/* An IPC call NR (Recv, Reply, ReplyRecv) on SLOT: registers R and the
   capabilities and word count in *R9 go out and come back, the received
   badge in *BADGE.  */
static uint64_t
joist_ipc (uint64_t nr, uint32_t slot, uint64_t timeout, uint64_t r[4],
	   uint64_t *r9, uint64_t *badge)
{
  register uint64_t rr9 __asm__ ("r9") = *r9;
  register uint64_t r12 __asm__ ("r12") = r[0];
  register uint64_t r13 __asm__ ("r13") = r[1];
  register uint64_t r14 __asm__ ("r14") = r[2];
  register uint64_t r15 __asm__ ("r15") = r[3];
  register uint64_t r10 __asm__ ("r10") = 0;
  register uint64_t r8 __asm__ ("r8") = 0;
  uint64_t rax = nr, rdi = slot, rsi = timeout, rdx = 0;
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
  *badge = rdx;
  return rax;
}

/* The answer's rights made, its words in the IPC buffer: 0, or a Mach
   error (nothing made).  */
static int
answer_prepare (struct jn_table *t, const struct joist_mig_msg *out,
		uint64_t *buf, uint32_t slot[], int temp[], uint64_t *r9)
{
  if (out->ncaps > JOIST_MIG_CAPS || out->len > JOIST_MIG_BUF_WORDS)
    return MACH_SEND_INVALID_HEADER;
  uint32_t n = 0;
  for (; n < out->ncaps; n++)
    if (__jn_send_prepare (t, out->cap[n], out->disp[n], &slot[n], &temp[n])
	!= JN_OK)
      {
	while (n-- > 0)
	  __jn_send_done (t, out->cap[n], out->disp[n], slot[n], temp[n], 0);
	return MACH_SEND_INVALID_RIGHT;
      }
  *r9 = n;
  for (uint32_t i = 0; i < n; i++)
    *r9 |= (uint64_t) slot[i] << (2 + 20 * i)
	   | mask_of (out->disp[i]) << (18 + 20 * i);
  *r9 |= (uint64_t) out->len << 48;
  if (out->len > 0)
    memcpy (buf, out->buf, (size_t) out->len * 8);
  return 0;
}

/* The Mach error of a receive's refusal.  */
static int
rcv_error_of (uint64_t e)
{
  switch (e)
    {
    case JOIST_E_TIMEOUT:
      return MACH_RCV_TIMED_OUT;
    case JOIST_E_ABORTED:
      return MACH_RCV_INTERRUPTED;
    default:			/* not a receive right or port set of ours */
      return MACH_RCV_INVALID_NAME;
    }
}

/* Answer OUT (if not NULL), then (if IN isn't NULL) receive on NAME for at
   most TIMEOUT nanoseconds (JOIST_FOREVER: no limit) into IN.  An answer
   with rights goes alone first (its rights' refusal must not be taken for
   the receive's); one without, with the receive in one step.  */
static int
server_ipc (struct joist_mig_msg *out, mach_port_t name,
	    struct joist_mig_msg *in, uint64_t timeout)
{
  uint32_t slot[JOIST_MIG_CAPS], land[JOIST_MIG_CAPS], recv = 0;
  int temp[JOIST_MIG_CAPS];
  uint64_t *buf = ipc_buffer ();
  if (buf == NULL)
    return MACH_SEND_NO_BUFFER;

  if (out != NULL && (in == NULL || out->ncaps > 0))
    {
      uint64_t r9 = 0, badge, r[4];
      struct jn_table *t = __joist_ports_lock ();
      int err = answer_prepare (t, out, buf, slot, temp, &r9);
      __joist_ports_unlock ();
      if (err != 0)
	return err;
      memcpy (r, out->r, sizeof r);
      uint64_t e = joist_ipc (JOIST_SYS_REPLY, 0, 0, r, &r9, &badge);
      int sent = e == JOIST_OK || e == JOIST_E_PEER;
      t = __joist_ports_lock ();
      for (uint32_t i = 0; i < out->ncaps; i++)
	__jn_send_done (t, out->cap[i], out->disp[i], slot[i], temp[i], sent);
      __joist_ports_unlock ();
      out = NULL;
      if (in == NULL)
	return 0;
    }

  /* The receive: its endpoint, and names for rights to land in.  */
  struct jn_table *t = __joist_ports_lock ();
  recv = __jn_recv_slot (t, name);
  uint32_t nland = 0;
  int err = recv == 0 ? MACH_RCV_INVALID_NAME : 0;
  while (err == 0 && nland < JOIST_MIG_CAPS)
    {
      if (__jn_landing (t, &land[nland]) != JN_OK)
	err = MACH_RCV_TOO_LARGE;
      else
	nland++;
    }
  uint64_t r9 = 0;
  if (err == 0 && out != NULL)
    err = answer_prepare (t, out, buf, slot, temp, &r9);	/* no rights */
  __joist_ports_unlock ();

  uint64_t e = JOIST_E_GONE, badge = 0;
  if (err == 0)
    {
      (void) __joist_sys (JOIST_SYS_RECV_SLOTS, land[0], land[1],
			  JOIST_MIG_CAPS, 0, NULL);
      uint64_t r[4] = { 0, 0, 0, 0 };
      if (out != NULL)
	memcpy (r, out->r, sizeof r);
      e = joist_ipc (out != NULL ? JOIST_SYS_REPLY_RECV : JOIST_SYS_RECV, recv,
		     timeout, r, &r9, &badge);
      if (e == JOIST_OK)
	memcpy (in->r, r, sizeof r);
      else
	err = rcv_error_of (e);
    }

  t = __joist_ports_lock ();
  uint32_t got = e == JOIST_OK ? (uint32_t) (r9 & 3) : 0;
  for (uint32_t i = 0; i < nland; i++)
    {
      uint32_t s = i < got ? (uint32_t) ((r9 >> (2 + 20 * i)) & 0xffff)
			   : JOIST_SLOT_NONE16;
      if (s != JOIST_SLOT_NONE16 && s == land[i])
	{
	  __jn_arrived (t, s);
	  in->cap[i] = s;
	}
      else
	{
	  __jn_unland (t, land[i]);
	  if (i < got)
	    in->cap[i] = MACH_PORT_NULL;
	}
    }
  __joist_ports_unlock ();
  if (err != 0)
    return err;
  in->ncaps = got;
  in->badge = badge;
  in->len = (uint32_t) ((r9 >> 48) & 0x3ff);
  if (in->len > JOIST_MIG_BUF_WORDS)
    in->len = 0;
  if (in->len > 0)
    memcpy (in->buf, buf, (size_t) in->len * 8);
  return 0;
}

int
__joist_mig_receive (mach_port_t name, struct joist_mig_msg *in,
		     uint64_t timeout)
{
  return server_ipc (NULL, name, in, timeout);
}
rtld_hidden_def (__joist_mig_receive)

int
__joist_mig_reply_receive (struct joist_mig_msg *out, mach_port_t name,
			   struct joist_mig_msg *in, uint64_t timeout)
{
  return server_ipc (out, name, in, timeout);
}
rtld_hidden_def (__joist_mig_reply_receive)

int
__joist_mig_reply (struct joist_mig_msg *out)
{
  return server_ipc (out, MACH_PORT_NULL, NULL, 0);
}
rtld_hidden_def (__joist_mig_reply)

/* No-senders for a port set (libports' notifier, the owner's design of
   2026-11-12): a notification bound to the set's endpoint, which the
   kernel signals when a port of the set loses its last sender (I4); the
   ports' badges collected one at a time.  */

#define JOIST_SYS_WAIT_NS	15
#define JOIST_SYS_NOSENDERS_NEXT 63
#define JOIST_SYS_BIND_NOSENDERS 64
#define JOIST_OBJ_NOTIFICATION_NS 2

int
__joist_nosenders_bind (mach_port_t set, mach_port_t *notif)
{
  struct jn_table *t = __joist_ports_lock ();
  uint32_t ep = __jn_recv_slot (t, set), r = 0, sig = 0;
  int ok = ep != 0 && __jn_pinned (t, &r) == JN_OK
	   && __jn_pinned (t, &sig) == JN_OK;
  __joist_ports_unlock ();
  ok = ok
       && __joist_sys (JOIST_SYS_OBJ_CREATE, JOIST_OBJ_NOTIFICATION_NS, r, 0,
		       0, NULL) == JOIST_OK
       && __joist_sys (JOIST_SYS_CAP_MINT, r, sig, JOIST_R_SEND, 1, NULL)
	  == JOIST_OK
       && __joist_sys (JOIST_SYS_BIND_NOSENDERS, ep, sig, 0, 0, NULL)
	  == JOIST_OK;
  /* The binding holds what it needs: the signalling capability goes.  */
  if (sig != 0)
    (void) __joist_sys (JOIST_SYS_CAP_DELETE, sig, 0, 0, 0, NULL);
  t = __joist_ports_lock ();
  if (sig != 0)
    __jn_unpin (t, sig);
  if (!ok && r != 0)
    {
      __joist_ports_unlock ();
      (void) __joist_sys (JOIST_SYS_CAP_DELETE, r, 0, 0, 0, NULL);
      t = __joist_ports_lock ();
      __jn_unpin (t, r);
    }
  __joist_ports_unlock ();
  if (!ok)
    return ep == 0 ? KERN_INVALID_NAME : KERN_RESOURCE_SHORTAGE;
  *notif = r;
  return KERN_SUCCESS;
}
rtld_hidden_def (__joist_nosenders_bind)

int
__joist_nosenders_wait (mach_port_t notif)
{
  uint64_t bits = 0;
  uint64_t e = __joist_sys (JOIST_SYS_WAIT_NS, notif, JOIST_FOREVER, 0, 0,
			    &bits);
  return e == JOIST_OK ? 0 : e == JOIST_E_ABORTED ? EINTR : EINVAL;
}
rtld_hidden_def (__joist_nosenders_wait)

int
__joist_nosenders_next (mach_port_t set, uint64_t *badge)
{
  struct jn_table *t = __joist_ports_lock ();
  uint32_t ep = __jn_recv_slot (t, set);
  __joist_ports_unlock ();
  if (ep == 0)
    return KERN_INVALID_NAME;
  *badge = 0;
  return __joist_sys (JOIST_SYS_NOSENDERS_NEXT, ep, 0, 0, 0, badge) == JOIST_OK
	 ? KERN_SUCCESS : KERN_INVALID_NAME;
}
rtld_hidden_def (__joist_nosenders_next)

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
