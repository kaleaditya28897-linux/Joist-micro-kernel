/* isr.S - interrupt and exception entry stubs (Phase 2 step 2).
 *
 * One stub per vector. Vectors where the CPU pushes an error code (8, 10-14,
 * 17, 21, 29, 30) push only the vector number; the rest push a zero first so
 * every frame has the same shape (struct trap_frame in cpu.h). The common
 * path saves all general registers, calls isr_dispatch(frame) with a 16-byte
 * aligned stack, restores, drops vector and error code, and returns with
 * iretq. No swapgs yet: nothing runs in user mode until step 6, which adds it.
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
