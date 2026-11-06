/* Thread state on Joist (Phase 5 step 7, the design's item 7): Mach's
   x86-64 flavors read from and written to Joist's one register record
   (decision 0010: pc, sp, flags, the TLS base, the general registers).
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

#ifndef _JOIST_STATE_H
#define _JOIST_STATE_H 1

#include <stdint.h>
#include <string.h>
#include <mach.h>
#include <mach/machine/thread_status.h>
#include "joist-sys.h"

#define JOIST_SYS_THREAD_READ_REGS	70
#define JOIST_SYS_THREAD_WRITE_REGS	71

/* Joist's register record (user/lib/joist.h's struct j_thread_regs).  */
enum { JR_RAX, JR_RBX, JR_RCX, JR_RDX, JR_RSI, JR_RDI, JR_RBP, JR_R8, JR_R9,
       JR_R10, JR_R11, JR_R12, JR_R13, JR_R14, JR_R15, JR_N = 16 };
struct joist_regs
{
  uint64_t pc, sp, flags, tls;
  uint64_t r[JR_N];
};

static inline void
joist_regs_to_mach (const struct joist_regs *j, struct i386_thread_state *m)
{
  memset (m, 0, sizeof *m);
  m->r8 = j->r[JR_R8];
  m->r9 = j->r[JR_R9];
  m->r10 = j->r[JR_R10];
  m->r11 = j->r[JR_R11];
  m->r12 = j->r[JR_R12];
  m->r13 = j->r[JR_R13];
  m->r14 = j->r[JR_R14];
  m->r15 = j->r[JR_R15];
  m->rdi = j->r[JR_RDI];
  m->rsi = j->r[JR_RSI];
  m->rbp = j->r[JR_RBP];
  m->rbx = j->r[JR_RBX];
  m->rdx = j->r[JR_RDX];
  m->rcx = j->r[JR_RCX];
  m->rax = j->r[JR_RAX];
  m->rip = j->pc;
  m->rsp = m->ursp = j->sp;	/* x86-64 Mach: ursp is the user's */
  m->rfl = j->flags;
}

static inline void
joist_regs_from_mach (struct joist_regs *j, const struct i386_thread_state *m)
{
  j->r[JR_R8] = m->r8;
  j->r[JR_R9] = m->r9;
  j->r[JR_R10] = m->r10;
  j->r[JR_R11] = m->r11;
  j->r[JR_R12] = m->r12;
  j->r[JR_R13] = m->r13;
  j->r[JR_R14] = m->r14;
  j->r[JR_R15] = m->r15;
  j->r[JR_RDI] = m->rdi;
  j->r[JR_RSI] = m->rsi;
  j->r[JR_RBP] = m->rbp;
  j->r[JR_RBX] = m->rbx;
  j->r[JR_RDX] = m->rdx;
  j->r[JR_RCX] = m->rcx;
  j->r[JR_RAX] = m->rax;
  j->pc = m->rip;
  j->sp = m->ursp;
  j->flags = m->rfl;
}

static inline kern_return_t
joist_regs_read (mach_port_t thread, struct joist_regs *j)
{
  return __joist_sys (JOIST_SYS_THREAD_READ_REGS, thread, (uintptr_t) j, 0,
		      0, NULL) == JOIST_OK ? KERN_SUCCESS : KERN_INVALID_ARGUMENT;
}

#endif
