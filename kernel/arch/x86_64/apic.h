/* SPDX-License-Identifier: GPL-3.0-only */
/* apic.h - the Local APIC timer (Phase 2 step 5). */
#ifndef JOIST_ARCH_APIC_H
#define JOIST_ARCH_APIC_H

#include <stdint.h>

#define APIC_TIMER_VECTOR    32u
/* Between CPUs (Phase 3 step 9): "choose again" (another thread deserves
 * this CPU), and "come into the kernel now" (to give up a thread, or to
 * leave an address space whose pages or tables are going). */
#define APIC_IPI_RESCHED     33u
#define APIC_IPI_SYNC        34u
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

/* Nanoseconds since the timer was calibrated: the TSC, measured against the
 * PIT in the same 10 ms as the LAPIC timer (Phase 3 step 5). Monotonic. */
uint64_t apic_clock_ns(void);
/* 1 if CPUID says the TSC is invariant (one rate, in every power state:
 * the clock above can be trusted to keep going); 0 if not, or not said. */
int apic_tsc_invariant(void);

/* From the periodic boot tick to one-shot use: after this the timer fires
 * only when armed. */
void apic_timer_oneshot(void);

/* Fire once, ns from now. Never sooner than one timer count; if ns is more
 * than the 32-bit counter holds (about a minute) it fires at that limit,
 * and the caller, finding nothing due, arms it again. */
void apic_timer_arm(uint64_t ns);

/* Don't fire (nothing is due). */
void apic_timer_disarm(void);

/* More than one CPU (Phase 3 step 9). End an interrupt other than the
 * timer's; this CPU's LAPIC id; bring up the LAPIC of a CPU after the first
 * (one-shot timer, the first CPU's calibration); send an interrupt to the
 * CPU with that LAPIC id. */
void apic_eoi(void);
uint32_t apic_id(void);
void apic_init_ap(void);
void apic_send_ipi(uint32_t lapic_id, uint32_t vector);

#endif /* JOIST_ARCH_APIC_H */
