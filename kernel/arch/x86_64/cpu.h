/* cpu.h - x86_64 CPU structures and the trap frame (Phase 2 step 2).
 * Selector layout is fixed for `syscall`/`sysret` (step 6): STAR's sysret
 * base is GDT_KDATA, so sysret loads SS = base + 8 (user data) and CS =
 * base + 16 (user code), both with RPL 3. */
#ifndef JOIST_ARCH_CPU_H
#define JOIST_ARCH_CPU_H

#include <stdint.h>

#define GDT_KCODE 0x08u
#define GDT_KDATA 0x10u
#define GDT_UDATA 0x18u /* | 3 when loaded */
#define GDT_UCODE 0x20u /* | 3 when loaded */
#define GDT_TSS   0x28u /* 16-byte system descriptor */

/* Interrupt stack table slots (1-based, as in the IDT gate). */
#define IST_DOUBLE_FAULT 1u

/* What isr.S pushes, lowest address first, then what the CPU pushed. */
struct trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error; /* the CPU's error code, or 0 for vectors without one */
    uint64_t rip, cs, rflags, rsp, ss;
};

void gdt_init(void);

/* End the run: report code through QEMU's isa-debug-exit device (0 = success)
 * and halt. boot.c. */
__attribute__((noreturn)) void kernel_finish(uint8_t code);
void idt_init(void);

/* Called by isr.S for every interrupt and exception. */
void isr_dispatch(struct trap_frame *tf);

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

#endif /* JOIST_ARCH_CPU_H */
