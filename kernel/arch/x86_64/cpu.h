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

/* The vector syscall.S gives a system call's frame (past every real one). */
#define SYSCALL_VECTOR 256u

/* Interrupt stack table slots (1-based, as in the IDT gate): what can't be
 * masked arrives on a stack of its own, one each, per CPU, each with a guard
 * page under it (gdt.c). */
#define IST_DOUBLE_FAULT  1u
#define IST_NMI           2u
#define IST_MACHINE_CHECK 3u
#define IST_COUNT         3u

/* What isr.S pushes, lowest address first, then what the CPU pushed. */
struct trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error; /* the CPU's error code, or 0 for vectors without one */
    uint64_t rip, cs, rflags, rsp, ss;
};

/* CPUs the GDT has a TSS for. */
#define GDT_MAX_CPUS 8u

/* The first CPU, at boot; any CPU, with its kernel stack (step 9); and a
 * new stack for a CPU's entries from user mode. */
void gdt_init(void);
void gdt_init_cpu(uint32_t cpu, uint64_t stack_top);
void gdt_set_stack(uint32_t cpu, uint64_t stack_top);
/* Unmap the guard page under every CPU's IST stacks (once the kernel's page
 * tables are loaded): NULL, or what failed. */
const char *gdt_guard_ist(void);
/* The lowest usable address of a CPU's IST stack (ist 1..IST_COUNT): the
 * guard page ends just under it. */
uint64_t gdt_ist_bottom(uint32_t cpu, uint32_t ist);
/* Which IST stack, of any CPU, holds addr: 1..IST_COUNT, or 0 for none. */
uint32_t gdt_ist_of(uint64_t addr);

/* End the run: report code through QEMU's isa-debug-exit device (0 = success)
 * and halt. boot.c. */
__attribute__((noreturn)) void kernel_finish(uint8_t code);

/* A trap's name, for reports (idt.c). */
const char *trap_name(uint64_t vector);
void idt_init(void);
/* Load the table on a CPU after the first. */
void idt_load(void);

/* Called by isr.S for every interrupt and exception. */
void isr_dispatch(struct trap_frame *tf);

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

#endif /* JOIST_ARCH_CPU_H */
