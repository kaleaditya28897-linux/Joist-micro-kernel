/* apic.h - the Local APIC timer (Phase 2 step 5). */
#ifndef JOIST_ARCH_APIC_H
#define JOIST_ARCH_APIC_H

#include <stdint.h>

#define APIC_TIMER_VECTOR    32u
#define APIC_SPURIOUS_VECTOR 0xffu
#define APIC_TICK_HZ         100u

/* Map the LAPIC, mask the legacy PIC, calibrate the timer against the PIT
 * and start it periodic at APIC_TICK_HZ. NULL on success, else what failed.
 * Interrupts stay disabled; the caller enables them. */
const char *apic_init(uint64_t hhdm);

/* Ticks since the timer started. */
uint64_t apic_ticks(void);

/* LAPIC timer counts per tick, as calibrated (for the boot log). */
uint32_t apic_counts_per_tick(void);

/* Called from the interrupt dispatcher for the timer vector. */
void apic_timer_interrupt(void);

#endif /* JOIST_ARCH_APIC_H */
