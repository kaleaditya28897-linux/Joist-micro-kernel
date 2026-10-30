/* SPDX-License-Identifier: GPL-3.0-only */
/* isr.S - interrupt and exception entry stubs (Phase 2 step 2).
 *
 * One stub per vector. Vectors where the CPU pushes an error code (8, 10-14,
 * 17, 21, 29, 30) push only the vector number; the rest push a zero first so
 * every frame has the same shape (struct trap_frame in cpu.h). The common
 * path saves all general registers, calls isr_dispatch(frame) with a 16-byte
 * aligned stack, restores, drops vector and error code, and returns with
 * iretq. No swapgs: GS base is the user's whenever the kernel runs
 * (syscall.S). From user mode, isr_dispatch doesn't return: the kernel goes
 * back to user mode through resume_user, below.
 */
    .text

.macro STUB vec
    .globl isr_\vec
isr_\vec:
    .if (\vec == 8) || (\vec >= 10 && \vec <= 14) || (\vec == 17) || (\vec == 21) || (\vec == 29) || (\vec == 30)
    .else
    pushq $0
    .endif
    pushq $\vec
    jmp isr_common
.endm

    .globl isr_common
isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    movq %rsp, %rdi        /* the trap frame */
    movq %rsp, %rbx        /* callee-saved: the unaligned stack pointer */
    andq $-16, %rsp        /* SysV: 16-byte aligned at the call */
    call isr_dispatch
    movq %rbx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp         /* vector and error code */
    iretq

/* resume_user(frame): enter user mode from a trap frame the kernel built
 * from a thread's record (run.c), at the top of the kernel stack: the stack
 * pointer moves to it, so whatever was on the stack below is abandoned (the
 * kernel is non-preemptible and keeps no state on its stack between entries,
 * decision 0016). */
    .globl resume_user
resume_user:
    movq %rdi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp         /* vector and error code */
    iretq

/* resume_user_sysret(frame): the same, for a thread whose last entry was
 * a system call and whose registers nobody has rewritten since: RCX and R11
 * are the call's to clobber, so sysretq can carry the RIP and RFLAGS, and
 * sysretq, unlike iretq, doesn't stop the pipeline. run.c has checked that
 * RIP is a user address (sysretq to a non-canonical one faults in ring 0)
 * and built RFLAGS itself. Between loading the user's RSP and sysretq the
 * kernel runs on the user's stack: interrupts are off, and the exceptions
 * that can't be masked (NMI, machine check, double fault) have a stack of
 * their own (idt.c). */
    .globl resume_user_sysret
resume_user_sysret:
    movq %rdi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    addq $8, %rsp          /* r11: RFLAGS goes there */
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    addq $8, %rsp          /* rcx: RIP goes there */
    popq %rbx
    popq %rax
    addq $16, %rsp         /* vector and error code */
    popq %rcx              /* RIP */
    addq $8, %rsp          /* CS */
    popq %r11              /* RFLAGS */
    popq %rsp              /* the user's stack */
    sysretq

/* One stub per vector, generated with a counter (the assembler limits
 * macro recursion, so no recursive generator). */
    .altmacro
    .set vec, 0
    .rept 256
    STUB %vec
    .set vec, vec + 1
    .endr

/* The table idt.c reads: the address of each stub, by vector. */
.macro ADDR v
    .quad isr_\v
.endm
    .section .rodata
    .globl isr_table
    .balign 8
isr_table:
    .set vec, 0
    .rept 256
    ADDR %vec
    .set vec, vec + 1
    .endr

    .section .note.GNU-stack,"",@progbits
