/* idt.c - the IDT and the exception dispatcher (Phase 2 step 2).
 *
 * Every vector has a stub (isr.S). An exception in the kernel is a kernel
 * bug: it is reported with the whole register state and the run ends with a
 * failure verdict. The one exception is #BP (int3), a trap that returns,
 * which is how the interrupt-return path is tested. Exceptions from user mode
 * come with step 6; device interrupts with step 5. */
#include "apic.h"
#include "cpu.h"
#include "run.h"
#include "joist/kprint.h"

extern const uint64_t isr_table[256];

struct __attribute__((packed)) idt_gate {
    uint16_t off0;
    uint16_t selector;
    uint8_t ist;
    uint8_t type; /* present | DPL | type (0xe: 64-bit interrupt gate) */
    uint16_t off1;
    uint32_t off2;
    uint32_t zero;
};

struct __attribute__((packed)) idtr {
    uint16_t limit;
    uint64_t base;
};

static struct idt_gate idt[256];

static const char *const names[32] = {"#DE divide error",
                                      "#DB debug",
                                      "NMI",
                                      "#BP breakpoint",
                                      "#OF overflow",
                                      "#BR bound range",
                                      "#UD invalid opcode",
                                      "#NM no FPU",
                                      "#DF double fault",
                                      "coprocessor overrun",
                                      "#TS invalid TSS",
                                      "#NP segment not present",
                                      "#SS stack fault",
                                      "#GP general protection",
                                      "#PF page fault",
                                      "reserved 15",
                                      "#MF x87 error",
                                      "#AC alignment check",
                                      "#MC machine check",
                                      "#XM SIMD error",
                                      "#VE virtualisation",
                                      "#CP control protection",
                                      "reserved 22",
                                      "reserved 23",
                                      "reserved 24",
                                      "reserved 25",
                                      "reserved 26",
                                      "reserved 27",
                                      "#HV hypervisor injection",
                                      "#VC VMM communication",
                                      "#SX security",
                                      "reserved 31"};

const char *trap_name(uint64_t vector)
{
    return vector < 32u ? names[vector] : "interrupt";
}

void idt_init(void)
{
    for (unsigned v = 0; v < 256u; v++) {
        uint64_t a = isr_table[v];
        idt[v] = (struct idt_gate){.off0 = (uint16_t)a,
                                   .selector = GDT_KCODE,
                                   /* What can't be masked arrives on a stack of
                                    * its own, whatever RSP held: the double
                                    * fault, and NMI and machine check, which
                                    * could otherwise land in the instant the
                                    * sysret exit runs on a user stack. All
                                    * three end the run with a report. */
                                   .ist = v == 8u || v == 2u || v == 18u ? (uint8_t)IST_DOUBLE_FAULT : 0u,
                                   .type = 0x8e, /* present, DPL 0, interrupt gate */
                                   .off1 = (uint16_t)(a >> 16),
                                   .off2 = (uint32_t)(a >> 32),
                                   .zero = 0};
    }
    struct idtr r = {.limit = (uint16_t)(sizeof idt - 1u), .base = (uint64_t)(uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(r) : "memory");
}

static void reg(const char *name, uint64_t v)
{
    kputs(name);
    kputx(v);
}

static void dump(const struct trap_frame *tf)
{
    reg("  rip ", tf->rip);
    reg("  cs ", tf->cs);
    reg("  rflags ", tf->rflags);
    reg("\n  rsp ", tf->rsp);
    reg("  ss ", tf->ss);
    reg("  error ", tf->error);
    reg("\n  rax ", tf->rax);
    reg("  rbx ", tf->rbx);
    reg("  rcx ", tf->rcx);
    reg("\n  rdx ", tf->rdx);
    reg("  rsi ", tf->rsi);
    reg("  rdi ", tf->rdi);
    reg("\n  rbp ", tf->rbp);
    reg("  r8 ", tf->r8);
    reg("  r9 ", tf->r9);
    reg("\n  r10 ", tf->r10);
    reg("  r11 ", tf->r11);
    reg("  r12 ", tf->r12);
    reg("\n  r13 ", tf->r13);
    reg("  r14 ", tf->r14);
    reg("  r15 ", tf->r15);
    kputs("\n");
}

/* UBSan in trap mode (the kernel's debug build, §8.1) turns a failed check
 * into `ud1l` with the check's kind in the instruction: 67 0f b9, then ModRM
 * 0x00 (kind 0) or 0x40 followed by the kind. The numbers are Clang's
 * sanitizer-handler list (clang 21); the common ones are named. Returns the
 * kind, or -1 if the instruction at rip isn't a UBSan trap. */
static int ubsan_kind(uint64_t rip)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)rip;
    if (p[0] != 0x67u || p[1] != 0x0fu || p[2] != 0xb9u) {
        return -1;
    }
    if (p[3] == 0x00u) {
        return 0;
    }
    return p[3] == 0x40u ? (int)p[4] : -1;
}

static const char *ubsan_name(int kind)
{
    switch (kind) {
    case 0:
        return "signed add overflow";
    case 3:
        return "division or remainder overflow";
    case 12:
        return "signed multiply overflow";
    case 13:
        return "negation overflow";
    case 18:
        return "index out of bounds";
    case 19:
        return "pointer overflow";
    case 20:
        return "shift out of bounds";
    case 21:
        return "signed subtract overflow";
    case 22:
        return "type mismatch (misaligned or null)";
    default:
        return "check";
    }
}

void isr_dispatch(struct trap_frame *tf)
{
    uint64_t v = tf->vector;
    if (v == 3u) {
        kputs("joist: #BP breakpoint at ");
        kputx(tf->rip);
        kputs(", continuing\n");
        return; /* a trap: rip already points past int3 */
    }
    if ((tf->cs & 3u) == 3u) {
        run_user_trap(tf); /* from user mode, a system call included: never returns */
    }
    if (v == APIC_TIMER_VECTOR) {
        run_tick(); /* while the kernel idles */
        return;
    }
    if (v == APIC_SPURIOUS_VECTOR) {
        return; /* no EOI for a spurious interrupt */
    }
    if (v == 6u && (tf->cs & 3u) == 0 && ubsan_kind(tf->rip) >= 0) {
        int kind = ubsan_kind(tf->rip);
        kputs("joist: UBSAN: ");
        kputs(ubsan_name(kind));
        kputs(" (kind ");
        kputu((uint64_t)kind);
        kputs(") at ");
        kputx(tf->rip);
        kputs("\n");
        kernel_finish(2);
    }
    if (v < 32u) {
        kputs("joist: EXCEPTION ");
        kputu(v);
        kputs(" (");
        kputs(names[v]);
        kputs(") in the kernel\n");
        if (v == 14u) {
            reg("  cr2 ", read_cr2());
            kputs("\n");
        }
        dump(tf);
        kernel_finish(2);
    }
    kputs("joist: unexpected interrupt ");
    kputu(v);
    kputs(", ignored\n");
}
