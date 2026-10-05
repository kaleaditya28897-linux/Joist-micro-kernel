/* apic.c - the Local APIC timer, calibrated against the PIT (Phase 2 step 5).
 *
 * Limine (base revision 5+) leaves the LAPIC enabled, with the spurious
 * vector at 0xff, the task priority at 0 and the LVT entries masked. The
 * kernel maps the LAPIC's registers uncached (they are device memory, which
 * the direct map leaves out), masks the legacy 8259 PIC, measures how many
 * timer counts fit in 10 ms of PIT channel 2, and runs the timer periodic. */
#include "apic.h"

#include "io.h"
#include "paging.h"

#define MSR_APIC_BASE  0x1bu
#define LAPIC_EOI      0x0b0u
#define LAPIC_SVR      0x0f0u
#define LAPIC_LVT_TMR  0x320u
#define LAPIC_TMR_INIT 0x380u
#define LAPIC_TMR_CUR  0x390u
#define LAPIC_TMR_DIV  0x3e0u

#define PIT_HZ 1193182u

static volatile uint32_t *lapic;
static volatile uint64_t ticks;
static uint32_t counts_per_tick;

static uint32_t rd(uint32_t reg)
{
    return lapic[reg / 4u];
}

static void wr(uint32_t reg, uint32_t v)
{
    lapic[reg / 4u] = v;
}

/* Run PIT channel 2 once for `ms` milliseconds (mode 0, gated by port 0x61)
 * and return how far the LAPIC timer counted down meanwhile. Polled, with a
 * bound so a missing PIT can't hang the boot. */
static uint32_t lapic_counts_during_pit(unsigned ms)
{
    uint32_t pit = PIT_HZ / 1000u * ms;
    uint8_t gate = inb(0x61);
    outb(0x61, (uint8_t)((gate & 0xfdu) | 0x01u)); /* gate on, speaker off */
    outb(0x43, 0xb0);                              /* channel 2, lobyte/hibyte, mode 0 */
    outb(0x42, (uint8_t)pit);
    outb(0x42, (uint8_t)(pit >> 8));
    /* Restart the count by toggling the gate. */
    gate = inb(0x61);
    outb(0x61, (uint8_t)(gate & 0xfeu));
    outb(0x61, (uint8_t)(gate | 0x01u));
    wr(LAPIC_TMR_INIT, 0xffffffffu);
    for (uint32_t spin = 0; spin < 100000000u && (inb(0x61) & 0x20u) == 0; spin++) {
    }
    uint32_t left = rd(LAPIC_TMR_CUR);
    wr(LAPIC_TMR_INIT, 0);
    return 0xffffffffu - left;
}

const char *apic_init(uint64_t hhdm)
{
    /* Mask the legacy PIC: all of its interrupts come through the I/O APIC
     * later, if at all. */
    outb(0x21, 0xff);
    outb(0xa1, 0xff);

    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_APIC_BASE));
    uint64_t base = ((uint64_t)hi << 32 | lo) & 0x000ffffffffff000ull;
    if ((lo & (1u << 11)) == 0) {
        return "the local APIC is disabled";
    }
    if (paging_map_device(hhdm + base, base, 0x1000) != KERR_OK) {
        return "could not map the local APIC";
    }
    lapic = (volatile uint32_t *)(uintptr_t)(hhdm + base);

    wr(LAPIC_SVR, 0x100u | APIC_SPURIOUS_VECTOR); /* software-enabled */
    wr(LAPIC_TMR_DIV, 0x3);                       /* divide by 16 */
    wr(LAPIC_LVT_TMR, 1u << 16);                  /* masked, one-shot, while measuring */
    uint32_t per10ms = lapic_counts_during_pit(10);
    if (per10ms == 0) {
        return "the LAPIC timer didn't count against the PIT";
    }
    counts_per_tick = per10ms * (100u / APIC_TICK_HZ);
    wr(LAPIC_LVT_TMR, APIC_TIMER_VECTOR | (1u << 17)); /* periodic, unmasked */
    wr(LAPIC_TMR_INIT, counts_per_tick);
    return NULL;
}

void apic_timer_interrupt(void)
{
    ticks++;
    wr(LAPIC_EOI, 0);
}

uint64_t apic_ticks(void)
{
    return ticks;
}

uint32_t apic_counts_per_tick(void)
{
    return counts_per_tick;
}
