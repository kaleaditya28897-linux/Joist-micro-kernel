/* syscall.S - the `syscall` entry and the first entry to user mode (Phase 2
 * step 6).
 *
 * syscall_entry runs with IF, DF, TF and AC cleared (SFMASK), RCX = the user
 * RIP after the instruction, R11 = the user RFLAGS, and the user's RSP and
 * GS. swapgs gives the per-CPU block (KERNEL_GS_BASE): offset 0 is the kernel
 * stack top, offset 8 a slot for the user RSP. The whole user register state
 * is saved as a struct syscall_frame (user.h) and handed to syscall_handler;
 * every register but RAX (the result) is restored from it, so nothing of the
 * kernel's leaks out. The sysret hazard (a non-canonical RCX faults in ring 0
 * on the user's stack) is refused in syscall_handler, before this returns.
 *
 * GS convention: in user mode GS base is the user's (0) and KERNEL_GS_BASE
 * the per-CPU block; only this entry swaps them. The interrupt and exception
 * path (isr.S) never uses GS, so it doesn't swap.
 */
    .text
    .globl syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:8          /* user RSP */
    movq %gs:0, %rsp          /* kernel stack */
    pushq %gs:8               /* frame: user_rsp */
    pushq %r11                /* rflags */
    pushq %rcx                /* rip */
    pushq %rax
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi           /* the frame */
    cld                       /* 16 pushes from an aligned top: still aligned for the call */
    call syscall_handler      /* result in rax */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    popq %r10
    popq %r9
    popq %r8
    popq %rdx
    popq %rsi
    popq %rdi
    addq $8, %rsp             /* saved rax: the result replaces it */
    popq %rcx
    popq %r11
    popq %rsp                 /* user RSP */
    swapgs
    sysretq

/* user_enter(entry, stack): the first entry to ring 3, with iretq and every
 * general register zeroed. Interrupts on in user mode (RFLAGS.IF). */
    .globl user_enter
user_enter:
    pushq $0x1b               /* SS: user data | 3 */
    pushq %rsi                /* RSP */
    pushq $0x202              /* RFLAGS: IF */
    pushq $0x23               /* CS: user code | 3 */
    pushq %rdi                /* RIP */
    xorl %eax, %eax
    xorl %ebx, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    xorl %esi, %esi
    xorl %edi, %edi
    xorl %ebp, %ebp
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    xorl %r10d, %r10d
    xorl %r11d, %r11d
    xorl %r12d, %r12d
    xorl %r13d, %r13d
    xorl %r14d, %r14d
    xorl %r15d, %r15d
    iretq                     /* GS base is 0 and KERNEL_GS_BASE the per-CPU block (user.c) */

    .section .note.GNU-stack,"",@progbits
