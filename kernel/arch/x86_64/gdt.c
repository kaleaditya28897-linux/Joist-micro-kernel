/* gdt.c - the GDT and TSS (Phase 2 step 2). Replaces Limine's GDT. */
#include "cpu.h"
#include "paging.h"

#include <stddef.h>

struct __attribute__((packed)) tss {
    uint32_t reserved0;
    uint64_t rsp[3]; /* stack for entry from ring 0-2: rsp[0] when user mode traps */
    uint64_t reserved1;
    uint64_t ist[7]; /* interrupt stack table */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base; /* past the TSS limit: no I/O bitmap, so no user port access */
};

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

extern uint8_t kernel_stack[16384];
/* A TSS per CPU (each has its own stack for entries from user mode, and its
 * own for the exceptions that can't be masked), and so a TSS descriptor per
 * CPU in the one GDT they share. */
/* The IST stacks: for each CPU, one each for the double fault, NMI and
 * machine check (IST_*), so that one arriving while another is reported
 * doesn't overwrite it. Each is a guard page, unmapped by gdt_guard_ist,
 * then the stack: an overflow faults rather than running into the next. */
#define IST_GUARD 4096u
#define IST_STACK 8192u
static __attribute__((aligned(IST_GUARD))) uint8_t ist_stack[GDT_MAX_CPUS][IST_COUNT][IST_GUARD + IST_STACK];
static struct tss tss[GDT_MAX_CPUS];

/* Null, kernel code, kernel data, user data, user code, TSS (two slots).
 * Code descriptors: present, S=1, executable, readable, L=1 (64-bit);
 * data: present, S=1, writable. Base and limit are ignored in long mode. */
static uint64_t gdt[5 + 2 * GDT_MAX_CPUS] = {
    0,
    0x00af9a000000ffffull, /* kernel code, DPL 0 */
    0x00cf92000000ffffull, /* kernel data, DPL 0 */
    0x00cff2000000ffffull, /* user data, DPL 3 */
    0x00affa000000ffffull, /* user code, DPL 3 */
    0,
    0,
};

void gdt_init(void)
{
    gdt_init_cpu(0, (uint64_t)(uintptr_t)(kernel_stack + sizeof kernel_stack));
}

const char *gdt_guard_ist(void)
{
    for (uint32_t cpu = 0; cpu < GDT_MAX_CPUS; cpu++) {
        for (uint32_t k = 0; k < IST_COUNT; k++) {
            if (paging_guard((uint64_t)(uintptr_t)&ist_stack[cpu][k][0]) != KERR_OK) {
                return "could not unmap a guard page under an IST stack";
            }
        }
    }
    return NULL;
}

uint64_t gdt_ist_bottom(uint32_t cpu, uint32_t ist)
{
    return (uint64_t)(uintptr_t)&ist_stack[cpu][ist - 1u][IST_GUARD];
}

uint32_t gdt_ist_of(uint64_t addr)
{
    for (uint32_t cpu = 0; cpu < GDT_MAX_CPUS; cpu++) {
        for (uint32_t k = 0; k < IST_COUNT; k++) {
            if (addr >= gdt_ist_bottom(cpu, k + 1u) &&
                addr < (uint64_t)(uintptr_t)(ist_stack[cpu][k] + sizeof ist_stack[cpu][k])) {
                return k + 1u;
            }
        }
    }
    return 0;
}

void gdt_set_stack(uint32_t cpu, uint64_t stack_top)
{
    tss[cpu].rsp[0] = stack_top;
}

void gdt_init_cpu(uint32_t cpu, uint64_t stack_top)
{
    struct tss *t = &tss[cpu];
    t->rsp[0] = stack_top;
    for (uint32_t k = 0; k < IST_COUNT; k++) {
        t->ist[k] = (uint64_t)(uintptr_t)(ist_stack[cpu][k] + sizeof ist_stack[cpu][k]);
    }
    t->iomap_base = (uint16_t)sizeof *t;

    /* The TSS descriptor: type 0x9 (available 64-bit TSS), present. */
    uint64_t base = (uint64_t)(uintptr_t)t;
    uint64_t limit = sizeof *t - 1u;
    gdt[5u + 2u * cpu] = (limit & 0xffffu) | ((base & 0xffffffu) << 16) | (0x89ull << 40) |
                         ((limit & 0xf0000u) << 32) | ((base & 0xff000000u) << 32);
    gdt[6u + 2u * cpu] = base >> 32;

    struct gdtr r = {.limit = (uint16_t)(sizeof gdt - 1u), .base = (uint64_t)(uintptr_t)gdt};
    /* Load the GDT, reload the data segments, reload CS with a far return,
     * then load the task register. */
    __asm__ volatile("lgdt %0\n\t"
                     "mov %w1, %%ds\n\t"
                     "mov %w1, %%es\n\t"
                     "mov %w1, %%ss\n\t"
                     "xor %%eax, %%eax\n\t"
                     "mov %%ax, %%fs\n\t"
                     "mov %%ax, %%gs\n\t"
                     "pushq %2\n\t"
                     "leaq 1f(%%rip), %%rax\n\t"
                     "pushq %%rax\n\t"
                     "lretq\n"
                     "1:\n\t"
                     "ltr %w3"
                     :
                     : "m"(r), "r"((uint64_t)GDT_KDATA), "i"((uint64_t)GDT_KCODE),
                       "r"((uint64_t)(GDT_TSS + 16u * cpu))
                     : "rax", "memory");
}
