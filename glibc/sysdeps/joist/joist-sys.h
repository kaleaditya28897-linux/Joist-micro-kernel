/* Joist's kernel calls, as glibc makes them (Phase 5 step 7).
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

/* The encoding is Joist's docs/abi.md, "Encoding": RAX the call number,
   RDI, RSI, RDX, R10, R8 the arguments; RAX the result (0 is OK), RDX a
   returned value.  The kernel keeps every other register but R12-R15 and
   R9 (a message's) and R8 and R10 (some calls' results); the `syscall'
   instruction clobbers RCX and R11.  Only the calls sysdeps/joist makes
   are here; their numbers are the kernel's (kernel/sys/abi.h).  */

#ifndef _JOIST_SYS_H
#define _JOIST_SYS_H 1

#include <stddef.h>
#include <stdint.h>

#define JOIST_SYS_CALL		10
#define JOIST_SYS_OBJ_CREATE	20
#define JOIST_SYS_CAP_DERIVE	21
#define JOIST_SYS_CAP_MINT	22
#define JOIST_SYS_CAP_DELETE	23
#define JOIST_SYS_CSPACE_GROW	26
#define JOIST_SYS_CSPACE_SHRINK	27

/* Objects and rights (Joist's kernel/include/joist/core.h).  */
#define JOIST_OBJ_ENDPOINT	1
#define JOIST_R_SEND		0x1u
#define JOIST_R_RECV		0x2u
#define JOIST_R_GRANT		0x8u
#define JOIST_R_ALL		0xfu

/* Result codes.  */
#define JOIST_OK		0
#define JOIST_E_NOMEM		5

/* No deadline.  */
#define JOIST_FOREVER		UINT64_MAX

/* mm (Joist's servers/mm/mm_proto.h, decision 0023): its capability's
   fixed slot in every task, and the requests libc makes, each with the
   virtual page, the pages and the permissions in the message's words 1-3;
   the reply's word 0 is the result (0, or mm's error, 1-4).  */
#define JOIST_MM_SLOT		8u
#define JOIST_MM_MAP_ANON	0x10001u
#define JOIST_MM_UNMAP		0x10002u
#define JOIST_MM_PROTECT	0x10003u

static inline uint64_t
__joist_sys (uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
	     uint64_t a3, uint64_t *out)
{
  register uint64_t r10 __asm__ ("r10") = a3;
  register uint64_t r8 __asm__ ("r8") = 0;
  register uint64_t r9 __asm__ ("r9") = 0;
  uint64_t rax = nr, rdx = a2;
  __asm__ volatile ("syscall"
		    : "+a" (rax), "+D" (a0), "+S" (a1), "+d" (rdx),
		      "+r" (r10), "+r" (r8), "+r" (r9)
		    :
		    : "rcx", "r11", "r12", "r13", "r14", "r15", "memory");
  if (out != NULL)
    *out = rdx;
  return rax;
}

/* The clock call (56): the monotonic time in nanoseconds since boot, and
   the number of CPUs.  Always OK.  */
static inline void
__joist_clock (uint64_t *ns, uint64_t *cpus)
{
  register uint64_t r10 __asm__ ("r10") = 0;
  register uint64_t r8 __asm__ ("r8") = 0;
  register uint64_t r9 __asm__ ("r9") = 0;
  uint64_t rax = 56, rdi = 0, rsi = 0, rdx = 0;
  __asm__ volatile ("syscall"
		    : "+a" (rax), "+D" (rdi), "+S" (rsi), "+d" (rdx),
		      "+r" (r10), "+r" (r8), "+r" (r9)
		    :
		    : "rcx", "r11", "r12", "r13", "r14", "r15", "memory");
  *ns = rdx;
  if (cpus != NULL)
    *cpus = r10;
}

/* A Call on SLOT carrying no capabilities: MSG's four words go, the
   reply's come back in it.  The kernel's result.  */
static inline uint64_t
__joist_call (uint64_t slot, uint64_t msg[4], uint64_t timeout)
{
  register uint64_t r9 __asm__ ("r9") = 0;
  register uint64_t r12 __asm__ ("r12") = msg[0];
  register uint64_t r13 __asm__ ("r13") = msg[1];
  register uint64_t r14 __asm__ ("r14") = msg[2];
  register uint64_t r15 __asm__ ("r15") = msg[3];
  uint64_t rax = JOIST_SYS_CALL, rdx;
  __asm__ volatile ("syscall"
		    : "+a" (rax), "+D" (slot), "+S" (timeout), "=d" (rdx),
		      "+r" (r9), "+r" (r12), "+r" (r13), "+r" (r14),
		      "+r" (r15)
		    :
		    : "rcx", "r11", "r8", "r10", "memory");
  msg[0] = r12;
  msg[1] = r13;
  msg[2] = r14;
  msg[3] = r15;
  return rax;
}

#endif
