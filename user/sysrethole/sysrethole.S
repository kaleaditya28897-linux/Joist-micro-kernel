/* SPDX-License-Identifier: GPL-3.0-only */
/* sysrethole.S - Phase 2 self-test root task for the sysret hazard (step 7).
 *
 * Linked at the last page of the user half (sysrethole.ld): `syscall` sits in
 * the page's final two bytes, so its return address is 0x800000000000, the
 * first non-canonical address. sysret to it would fault in ring 0 on this
 * task's stack (the classic hole); the kernel must refuse and end the task.
 * Call 99 is unknown, so the call itself does nothing. */
    .text
    .globl _start
_start:
    movl $99, %eax
    jmp the_syscall
    .org 0xffe, 0xcc
the_syscall:
    syscall
