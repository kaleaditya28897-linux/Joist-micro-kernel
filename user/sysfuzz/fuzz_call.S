/* SPDX-License-Identifier: GPL-3.0-only */
/* fuzz_call.S - one system call with every general register chosen
 * (Phase 2 step 7). fuzz_call(in, out): load rax, rbx, rdx, rsi, rdi, rbp,
 * r8-r10, r12-r15 from *in, execute syscall, store the same registers to
 * *out. rcx and r11 are clobbered by the instruction itself; rsp is left
 * alone. The C caller's callee-saved registers are preserved. */
    .text
    .globl fuzz_call
fuzz_call:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rsi                /* out, at 0(%rsp) */
    movq 0(%rdi), %rax
    movq 8(%rdi), %rbx
    movq 16(%rdi), %rdx
    movq 24(%rdi), %rsi
    movq 40(%rdi), %rbp
    movq 48(%rdi), %r8
    movq 56(%rdi), %r9
    movq 64(%rdi), %r10
    movq 72(%rdi), %r12
    movq 80(%rdi), %r13
    movq 88(%rdi), %r14
    movq 96(%rdi), %r15
    movq 32(%rdi), %rdi       /* last: it held the pointer */
    syscall
    xchgq %rdi, 0(%rsp)       /* rdi = out; the returned rdi is on the stack */
    movq %rax, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rdx, 16(%rdi)
    movq %rsi, 24(%rdi)
    movq %rbp, 40(%rdi)
    movq %r8, 48(%rdi)
    movq %r9, 56(%rdi)
    movq %r10, 64(%rdi)
    movq %r12, 72(%rdi)
    movq %r13, 80(%rdi)
    movq %r14, 88(%rdi)
    movq %r15, 96(%rdi)
    movq 0(%rsp), %rax
    movq %rax, 32(%rdi)       /* the returned rdi */
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

    .section .note.GNU-stack,"",@progbits
