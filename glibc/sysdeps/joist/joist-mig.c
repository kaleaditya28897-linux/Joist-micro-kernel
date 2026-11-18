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

#include "joist-mem.h"
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
  joist_mig_nulls_out (m);	/* null ports: marked, not sent */

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
  (void) joist_mig_nulls_in (m);	/* the reply's null ports back in place */
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
#define JOIST_SYS_REPLY_SAVED	18

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

/* Send an answer: registers R, its rights and words prepared (R9), on the
   thread's reply capability, or through its reply object if the reply was
   saved (joist-sync.c's reply_save: the thread waited as it served).  ERR
   not 0 (its rights refused), or the kernel's refusal (the reply
   capability then still the thread's, or the object's): an error alone
   answers instead, so the caller never waits for ever and the thread's
   next receive isn't refused (the kernel's I10), which a server's loop
   would take for its port set gone.  The kernel's result for the answer
   (E_PEER: its caller gone, no error).

   Whether the reply is saved is read here, just before the system call:
   any lock taken before (the names lock, say) may have been waited for,
   and that wait saves the reply.  */
static uint64_t
answer_send (struct joist_thread *self, const uint64_t r_in[4], uint64_t r9,
	     int err)
{
  uint64_t badge, e = JOIST_E_GONE;
  int saved = self->reply_saved;
  uint64_t nr = saved ? JOIST_SYS_REPLY_SAVED : JOIST_SYS_REPLY;
  uint32_t obj = saved ? self->reply_obj : 0;
  if (err == 0)
    {
      uint64_t r[4];
      memcpy (r, r_in, sizeof r);
      e = joist_ipc (nr, obj, 0, r, &r9, &badge);
    }
  if (e != JOIST_OK && e != JOIST_E_PEER)
    {
      uint64_t r0[4] = { (uint32_t) (err != 0 ? err
				     : MACH_SEND_INVALID_REPLY), 0, 0, 0 };
      uint64_t r90 = 0;
      (void) joist_ipc (nr, obj, 0, r0, &r90, &badge);
    }
  self->reply_saved = 0;
  return e;
}

/* Answer OUT (if not NULL), then (if IN isn't NULL) receive on NAME for at
   most TIMEOUT nanoseconds (JOIST_FOREVER: no limit) into IN.  An answer
   with rights goes alone first (its rights' refusal must not be taken for
   the receive's); one without, with the receive in one step.  An answer
   whose reply was saved goes alone too (answer_send).  */
static int
server_ipc (struct joist_mig_msg *out, mach_port_t name,
	    struct joist_mig_msg *in, uint64_t timeout)
{
  uint32_t slot[JOIST_MIG_CAPS], land[JOIST_MIG_CAPS], recv = 0;
  int temp[JOIST_MIG_CAPS];
  uint64_t *buf = ipc_buffer ();
  if (buf == NULL)
    return MACH_SEND_NO_BUFFER;
  if (out != NULL)
    joist_mig_nulls_out (out);	/* the answer's null ports: marked, not sent */

  struct joist_thread *self = __joist_thread_self ();
  if (out == NULL && self->reply_obj == 0)
    /* A server thread's first receive (it holds no reply: it may wait):
       its reply object, for reply_save (joist-sync.c) to use later.  Not
       made, its waits while it serves stay refused (they return at once),
       as before reply_save.  */
    (void) __joist_reply_obj_make (self);
  if (out != NULL && (in == NULL || out->ncaps > 0 || self->reply_saved))
    {
      uint64_t r9 = 0;
      struct jn_table *t = __joist_ports_lock ();
      int err = answer_prepare (t, out, buf, slot, temp, &r9);
      __joist_ports_unlock ();
      uint64_t e = answer_send (self, out->r, r9, err);
      if (err != 0)
	return err;
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
  if (out != NULL && (err != 0 || self->reply_saved))
    {
      /* Not with the receive: refused (an error alone answers it), or its
	 reply saved as the lock above was waited for (it goes through the
	 reply object).  */
      (void) answer_send (self, out->r, r9, err);
      out = NULL;
      r9 = 0;
    }

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
  (void) joist_mig_nulls_in (in);	/* the request's null ports back in place */
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
__joist_nosenders_next (mach_port_t set, uint64_t *badge,
			mach_port_mscount_t *mscount)
{
  /* The firing taken and the count read under the table's lock, as every
     make-send counts and mints under it: a send right made before is
     counted (and, made after the firing, took it back: I4), one made
     after isn't, so libports sees a send right it handed out since (its
     own count past this one) and ignores the firing, as on Mach.  */
  struct jn_table *t = __joist_ports_lock ();
  uint32_t ep = __jn_recv_slot (t, set), ms = 0;
  uint64_t e = JOIST_E_GONE;
  *badge = 0;
  if (ep != 0)
    {
      e = __joist_sys (JOIST_SYS_NOSENDERS_NEXT, ep, 0, 0, 0, badge);
      if (e == JOIST_OK && *badge != 0)
	(void) __jn_mscount_by_badge (t, ep, *badge, &ms);	/* (gone: 0) */
    }
  __joist_ports_unlock ();
  *mscount = ms;
  return e == JOIST_OK ? KERN_SUCCESS : KERN_INVALID_NAME;
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

/* ---- bulk data through the caller's windows (Joist's decision 0032) ----

   The kernel's calls 86-88 (kernel/sys/abi.h).  A copy goes through the
   reply the thread holds for the call it serves: its own (the slot
   JOIST_SLOT_NONE32), or, if it waited while serving, the reply object it
   was saved into (joist-sync.c's reply_save): read at each copy, as
   answer_send reads it, since anything before may have waited.  */

#define JOIST_SYS_THREAD_WINDOW	86
#define JOIST_SYS_REPLY_READ	87
#define JOIST_SYS_REPLY_WRITE	88
#define JOIST_SLOT_NONE32	0xffffffffu

int
__joist_mig_window (unsigned int kind, const void *va, size_t bytes)
{
  return __joist_sys (JOIST_SYS_THREAD_WINDOW, kind, (uintptr_t) va, bytes,
		      0, NULL) == JOIST_OK ? 0 : -1;
}
rtld_hidden_def (__joist_mig_window)

/* The write window's memory (the owner's choice of 2026-11-16): a write
   window must lie in one of the task's regions (0030, 0032), and libc's
   vm_allocate memory is mm's demand-zero pages, no region.  So libc keeps
   one region for every thread's windows, made at the first need: 32 page
   tables (64 MiB, JW_PAGES) held in its address table (vm_allocate never
   places anything there; vm_deallocate and vm_protect of it are refused),
   then asked of mm (MM_REQ_REGION_NEW, writable; its capability, MAP
   alone, in a pinned name's slot).  It grows by extents of zeroed pages
   (MM_REQ_REGION_GROW), each mapped writable by the task itself
   (region_map), doubling (joist-winmem.h), only as far as a call needs;
   pages stay mapped once grown.  A call's window is a run of whole pages
   of the mapped part, first fit (joist-winmem.c), given back after the
   call.  One lock over it all, held across mm's requests: a call that
   needs the region grown waits for it either way.

   NULL when mm refuses (the region, an extent), when the region is full,
   or when as many slices are out as there may be: the stub then offers no
   window (in line, or MIG_ARRAY_TOO_LARGE past the message), as before.
   A refused request is asked again at the next need; an extent mm grew
   but the kernel refused to map stops the region's growth for good (its
   pages are mm's, counted against the task, until the task ends).

   fork: mm gives a fork's child its parent's regions, copied, at the same
   slots (0030), and the child's libc has its parent's table, address
   table and names, so this state holds in the child as it is; the slices
   other threads had out at the fork are never given back there (their
   threads aren't in the child): at most JW_RUNS - 1 slices' pages.  fork
   is refused in Phase 5 (_Fork.c); Phase 7's must take this lock across
   it, as it takes libc's others.  */

#include "joist-winmem.c"

#define JOIST_SYS_REGION_MAP	82
#define JOIST_MM_REGION_NEW	0x10009u
#define JOIST_MM_REGION_GROW	0x1000au
#define JOIST_MM_W		1u	/* mm's MM_W, the kernel's JOIST_MAP_W */
#define WIN_TABLE		512u	/* pages a page table maps */
#define WIN_PAGE_SHIFT		12

_Static_assert (JW_PAGES == 32u * WIN_TABLE,
		"the window region: 32 page tables");

static struct jw_table win;
static uint64_t win_first;	/* the region's first page: 0 until made */
static uint32_t win_slot;	/* the name holding its capability */
static int win_stuck;		/* an extent left unmapped: no more growth */
__libc_lock_define_initialized (static, win_lock)

/* region_map (82): pages [PAGE, PAGE + N) of extent EXT at virtual page
   VA, with PERMS (VA's in R8, the permissions in R9).  */
static uint64_t
region_map (uint32_t slot, uint64_t ext, uint64_t page, uint64_t n,
	    uint64_t va, uint64_t perms)
{
  register uint64_t r10 __asm__ ("r10") = n;
  register uint64_t r8 __asm__ ("r8") = va;
  register uint64_t r9 __asm__ ("r9") = perms;
  uint64_t rax = JOIST_SYS_REGION_MAP, rdi = slot, rsi = ext, rdx = page;
  __asm__ volatile ("syscall"
		    : "+a" (rax), "+D" (rdi), "+S" (rsi), "+d" (rdx),
		      "+r" (r10), "+r" (r8), "+r" (r9)
		    :
		    : "rcx", "r11", "r12", "r13", "r14", "r15", "memory");
  return rax;
}

/* A request to mm: its result (mm's, or -1 if the kernel refused the
   Call); its reply's second word in *R1.  */
static int
win_mm (uint64_t req, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t *r1)
{
  uint64_t msg[4] = { req, a1, a2, a3 };
  if (__joist_call (JOIST_MM_SLOT, msg, JOIST_FOREVER) != JOIST_OK)
    return -1;
  if (r1 != NULL)
    *r1 = msg[1];
  return msg[0] > INT32_MAX ? -1 : (int) msg[0];
}

/* The region, made if it isn't yet: 0, or -1 (nothing kept).  */
static int
win_region (void)
{
  if (win_first != 0)
    return 0;
  uint64_t first;
  struct jv_table *vt = __joist_vm_lock ();
  int e = __jv_hold (vt, JW_PAGES, WIN_TABLE, &first);
  __joist_vm_unlock ();
  if (e != JV_OK)
    return -1;
  uint32_t slot = 0;
  struct jn_table *t = __joist_ports_lock ();
  int ok = __jn_pinned (t, &slot) == JN_OK;
  __joist_ports_unlock ();
  ok = ok && win_mm (JOIST_MM_REGION_NEW, first,
		     (JW_PAGES / WIN_TABLE) | JOIST_MM_W << 8, slot, NULL) == 0;
  if (!ok)
    {
      /* mm puts the capability in the slot only when it made the region:
	 the name is empty, given back as it is.  */
      if (slot != 0)
	{
	  t = __joist_ports_lock ();
	  __jn_unpin (t, slot);
	  __joist_ports_unlock ();
	}
      vt = __joist_vm_lock ();
      (void) __jv_unhold (vt, first);
      __joist_vm_unlock ();
      return -1;
    }
  __jw_init (&win);
  win_slot = slot;
  win_first = first;
  return 0;
}

/* The next extent grown by mm and mapped here, writable, a page table at
   a time (a mapping can't cross one): 0, or -1.  */
static int
win_grow (void)
{
  uint32_t n = __jw_extent_pages (&win);
  uint64_t ext = 0;
  if (n == 0 || win_stuck
      || win_mm (JOIST_MM_REGION_GROW, win_slot, n, 0, &ext) != 0)
    return -1;
  uint64_t va = win_first + win.mapped;
  for (uint32_t at = 0; at < n;)
    {
      uint32_t k = WIN_TABLE - (uint32_t) ((va + at) % WIN_TABLE);
      if (k > n - at)
	k = n - at;
      if (region_map (win_slot, ext, at, k, va + at, JOIST_MM_W) != JOIST_OK)
	{
	  win_stuck = 1;
	  return -1;
	}
      at += k;
    }
  return __jw_grow (&win, n) == JW_OK ? 0 : -1;
}

void *
__joist_mig_window_allocate (size_t bytes)
{
  if (bytes == 0 || bytes > (size_t) JW_PAGES << WIN_PAGE_SHIFT)
    return NULL;
  uint32_t pages = (uint32_t) ((bytes + (1u << WIN_PAGE_SHIFT) - 1)
			       >> WIN_PAGE_SHIFT);
  void *p = NULL;
  uint32_t grow = 0, g = 0, at;
  __libc_lock_lock (win_lock);
  if (win_region () == 0 && __jw_plan (&win, pages, &grow) == JW_OK)
    {
      while (g < grow && win_grow () == 0)
	g++;
      if (g == grow && __jw_take (&win, pages, &at) == JW_OK)
	p = (void *) (uintptr_t) ((win_first + at) << WIN_PAGE_SHIFT);
    }
  __libc_lock_unlock (win_lock);
  return p;
}
rtld_hidden_def (__joist_mig_window_allocate)

void
__joist_mig_window_free (void *p, size_t bytes)
{
  if (p == NULL || bytes == 0)
    return;
  uint64_t page = (uintptr_t) p >> WIN_PAGE_SHIFT;
  uint32_t pages = (uint32_t) ((bytes + (1u << WIN_PAGE_SHIFT) - 1)
			       >> WIN_PAGE_SHIFT);
  __libc_lock_lock (win_lock);
  if (win_first != 0 && page >= win_first && page - win_first < JW_PAGES)
    (void) __jw_give (&win, (uint32_t) (page - win_first), pages);
  __libc_lock_unlock (win_lock);
}
rtld_hidden_def (__joist_mig_window_free)

static int
reply_copy (uint64_t nr, uint64_t off, uintptr_t va, size_t bytes,
	    size_t *done)
{
  struct joist_thread *self = __joist_thread_self ();
  uint32_t reply = self->reply_saved ? self->reply_obj : JOIST_SLOT_NONE32;
  uint64_t n = 0;
  uint64_t e = __joist_sys (nr, reply, off, va, bytes, &n);
  *done = e == JOIST_OK ? (size_t) n : 0;
  return e == JOIST_OK ? 0 : -1;
}

int
__joist_mig_reply_read (uint64_t off, void *dst, size_t bytes, size_t *done)
{
  return reply_copy (JOIST_SYS_REPLY_READ, off, (uintptr_t) dst, bytes,
		     done);
}
rtld_hidden_def (__joist_mig_reply_read)

int
__joist_mig_reply_write (uint64_t off, const void *src, size_t bytes,
			 size_t *done)
{
  return reply_copy (JOIST_SYS_REPLY_WRITE, off, (uintptr_t) src, bytes,
		     done);
}
rtld_hidden_def (__joist_mig_reply_write)

#endif
