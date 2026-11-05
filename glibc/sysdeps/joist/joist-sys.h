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

#endif
