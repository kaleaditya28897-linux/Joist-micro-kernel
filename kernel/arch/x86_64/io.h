/* io.h - x86 port I/O (Phase 2). Inline assembly is confined to arch/ (§5.1).
 * Preconditions: the caller is in ring 0 and the port belongs to a device the
 * kernel owns (COM1, the PIT, QEMU's debug-exit port). */
#ifndef JOIST_ARCH_IO_H
#define JOIST_ARCH_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

/* Stop this CPU for good: interrupts off, then hlt in a loop (an NMI can
 * still wake hlt). */
static inline __attribute__((noreturn)) void cpu_halt_forever(void)
{
    for (;;) {
        __asm__ volatile("cli; hlt" : : : "memory");
    }
}

#endif /* JOIST_ARCH_IO_H */
