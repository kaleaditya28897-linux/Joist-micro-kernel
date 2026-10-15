/* syscall.S - the `syscall` entry (Phase 2 step 6; reworked in Phase 3 step
 * 4).
 *
 * syscall_entry runs with IF, DF, TF and AC cleared (SFMASK), RCX = the user
 * RIP after the instruction, R11 = the user RFLAGS, and the user's RSP and
 * GS. swapgs gives the per-CPU block (KERNEL_GS_BASE): offset 0 is the kernel
 * stack top, offset 8 a slot for the user RSP. It builds the same frame the
 * CPU and isr.S build for an interrupt from user mode (struct trap_frame,
 * cpu.h: SS, RSP, RFLAGS, CS, RIP, error 0, vector SYSCALL_VECTOR), swaps GS
 * back, and joins isr.S's common path, so system calls and interrupts enter
 * and leave the kernel the same way. The way back to user mode is
 * resume_user (isr.S) with iretq, or resume_user_sysret with sysretq for a
 * thread whose last entry was a `syscall` (run.c chooses), from the thread
 * record of whichever thread runs next; its RIP is checked to be in the user
 * half first (run.c), which also covers what Phase 2's sysret guard did.
 *
 * GS convention: GS base is the user's (0) everywhere but the five
 * instructions below; the kernel never uses GS otherwise. */
    .text
    .globl syscall_entry
syscall_entry:
    swapgs
    movq %rsp, %gs:8          /* user RSP */
    movq %gs:0, %rsp          /* kernel stack */
    pushq $0x1b               /* SS: user data | 3 */
    pushq %gs:8               /* RSP */
    swapgs
    pushq %r11                /* RFLAGS */
    pushq $0x23               /* CS: user code | 3 */
    pushq %rcx                /* RIP */
    pushq $0                  /* error code */
    pushq $256                /* vector: SYSCALL_VECTOR (cpu.h) */
    jmp isr_common

    .section .note.GNU-stack,"",@progbits
