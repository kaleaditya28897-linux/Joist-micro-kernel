/* SPDX-License-Identifier: GPL-3.0-only */
/* boot.c - the kernel's entry from Limine (Phase 2 step 1; decision 0015).
 *
 * Limine enters kmain in 64-bit long mode, higher half, interrupts off, with a
 * valid stack and its own GDT and page tables, no IDT (protocol base revision
 * 6, x86-64 machine state). What the kernel needs from Limine it asks for with
 * the requests below, which sit in their own section between Limine's start
 * and end markers; Limine fills in their response pointers before entry. */
#include "apic.h"
#include "cpu.h"
#include "frame.h"
#include "io.h"
#include "paging.h"
#include "run.h"
#include "joist/kprint.h"
#include "serial.h"

#include <limine/limine.h>
#include <stddef.h>
#include <stdint.h>

#define REQ __attribute__((used, section(".limine_requests")))

__attribute__((used, section(".limine_requests_start"))) static volatile uint64_t req_start[4] =
    LIMINE_REQUESTS_START_MARKER;
REQ static volatile uint64_t base_revision[3] = LIMINE_BASE_REVISION(6);
REQ static volatile struct limine_memmap_request memmap_req = {.id = LIMINE_MEMMAP_REQUEST_ID,
                                                               .revision = 0};
REQ static volatile struct limine_hhdm_request hhdm_req = {.id = LIMINE_HHDM_REQUEST_ID,
                                                           .revision = 0};
REQ static volatile struct limine_executable_address_request kaddr_req = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0};
REQ static volatile struct limine_module_request module_req = {.id = LIMINE_MODULE_REQUEST_ID,
                                                               .revision = 0};
/* The other CPUs. Limine starts them and parks each in its own code (in
 * bootloader-reclaimable memory, which the frame allocator never takes),
 * waiting for a goto_address. Until SMP bring-up (Phase 3, decision 0016)
 * they stay parked; the kernel only counts them, and says so (0003). */
REQ static volatile struct limine_mp_request mp_req = {.id = LIMINE_MP_REQUEST_ID,
                                                       .revision = 0,
                                                       .flags = 0};
__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t req_end[2] =
    LIMINE_REQUESTS_END_MARKER;

/* The kernel's own stack (16 KiB, in BSS), for the TSS's RSP0 once user
 * mode exists (step 2); until then the kernel runs on Limine's. */
__attribute__((aligned(16))) uint8_t kernel_stack[16384];

/* QEMU's isa-debug-exit device (iobase 0xf4): QEMU exits with status
 * (code << 1) | 1, so `make qemu-test` sees the kernel's verdict. The code is
 * also printed, for runs where QEMU's exit status can't be seen (as on a
 * Proxmox VM until 2026-10-14), and tools/p53/boot.sh requires it to trust
 * a status. On hardware the port is unused and this just halts. */
__attribute__((noreturn)) void kernel_finish(uint8_t code)
{
    kputs("joist: finish code ");
    kputu(code);
    kputs("\n");
    outb(0xf4, code);
    cpu_halt_forever();
}

static __attribute__((noreturn)) void panic(const char *why)
{
    kputs("joist: PANIC: ");
    kputs(why);
    kputs("\n");
    kernel_finish(1);
}

/* The boot-time frame allocator (kernel/mem/frame.h), set up from Limine's
 * memory map. */
static struct frame_alloc frames;
#define MAX_USABLE 128u

/* Set up the frame allocator: collect the usable ranges, put both bitmaps
 * in the first usable range big enough (through the direct map), reserve
 * that storage. Panics rather than guessing if the map doesn't fit. */
static void frames_init(const struct limine_memmap_response *mm, uint64_t hhdm)
{
    static struct frame_range usable[MAX_USABLE];
    size_t n = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        const struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        if (n == MAX_USABLE) {
            panic("more usable memory ranges than the boot allocator takes");
        }
        usable[n++] = (struct frame_range){.base = e->base, .length = e->length};
    }
    size_t need = frame_bitmap_bytes(usable, n);
    if (need == 0) {
        panic("the memory map is malformed or has no usable memory");
    }
    uint64_t at = 0;
    int found = 0;
    for (size_t i = 0; i < n && !found; i++) {
        uint64_t base = (usable[i].base + FRAME_SIZE - 1u) & ~(uint64_t)(FRAME_SIZE - 1u);
        uint64_t end = usable[i].base + usable[i].length;
        if (base < end && end - base >= need) {
            at = base;
            found = 1;
        }
    }
    if (!found) {
        panic("no usable range holds the frame bitmaps");
    }
    if (frame_init(&frames, usable, n, (void *)(uintptr_t)(hhdm + at), need) != KERR_OK) {
        panic("frame_init refused the memory map");
    }
    frame_reserve(&frames, at, need);
}

/* Boot-time check: take a few frames, write and read them back through the
 * direct map, give them back; the free count must return exactly. */
static void frames_selfcheck(uint64_t hhdm)
{
    uint64_t before = frames.nfree;
    uint64_t got[8];
    for (unsigned i = 0; i < 8u; i++) {
        if (frame_get(&frames, &got[i]) != KERR_OK) {
            panic("frame_get failed with frames free");
        }
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)(hhdm + got[i]);
        p[0] = got[i] ^ 0x5a5a5a5a5a5a5a5aull;
        p[511] = ~got[i];
        if (p[0] != (got[i] ^ 0x5a5a5a5a5a5a5a5aull) || p[511] != ~got[i]) {
            panic("a frame doesn't hold what was written to it");
        }
    }
    for (unsigned i = 0; i < 8u; i++) {
        if (frame_put(&frames, got[i]) != KERR_OK) {
            panic("frame_put refused a frame it handed out");
        }
    }
    if (frames.nfree != before) {
        panic("the free count drifted");
    }
}

#if defined(JOIST_SELFTEST) && JOIST_SELFTEST == 4
static const char selftest_rodata[] = "read-only";
#endif
#if defined(JOIST_SELFTEST) && JOIST_SELFTEST == 5
static uint8_t selftest_code[16];
#endif

void kmain(void);

/* The stack protector (-fstack-protector-strong, Phase 3's exit): the canary
 * every protected function keeps below its locals, and what happens when one
 * comes back changed. The canary is chosen in kmain before any protected
 * function has run (kmain itself isn't protected), from RDRAND when the CPU
 * has it, else the time stamp counter. */
/* (The two names are the compiler's, so reserved identifiers by design.) */
uintptr_t __stack_chk_guard = 0x595e9fbd94fda766ull; // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
__attribute__((noreturn)) void __stack_chk_fail(void); // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

void __stack_chk_fail(void) // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
{
    kputs("joist: STACK SMASHED: a protected function's canary changed\n");
    kernel_finish(2);
}

static __attribute__((no_stack_protector)) uint64_t canary(void)
{
    uint32_t a = 1;
    uint32_t b;
    uint32_t c = 0;
    uint32_t d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    (void)b;
    (void)d;
    uint64_t v = 0;
    if (c & (1u << 30)) { /* RDRAND */
        for (int tries = 0; tries < 10; tries++) {
            uint8_t ok;
            __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
            if (ok) {
                return v;
            }
        }
    }
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    v = ((uint64_t)hi << 32 | lo) * 0x9E3779B97F4A7C15ull;
    return v ^ (v >> 29);
}

#if defined(JOIST_SELFTEST) && JOIST_SELFTEST == 9
/* Self-test 9: a protected function writes past its array. */
static __attribute__((noinline)) void smash(volatile size_t n)
{
    char buf[16];
    for (size_t i = 0; i < n; i++) {
        ((volatile char *)buf)[i] = 'A';
    }
}
#endif

__attribute__((no_stack_protector)) void kmain(void)
{
    __stack_chk_guard = (uintptr_t)canary();
    serial_init();
    kputs("\njoist: boot\n");
    gdt_init();
    idt_init();
    kputs("joist: gdt, tss, idt loaded\n");
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision)) {
        panic("the bootloader doesn't support Limine base revision 6");
    }
    if (memmap_req.response == NULL || hhdm_req.response == NULL || kaddr_req.response == NULL) {
        panic("a required boot request was not answered");
    }

    const struct limine_memmap_response *mm = memmap_req.response;
    uint64_t usable = 0;
    uint64_t ranges = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        const struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            usable += e->length;
            ranges++;
        }
    }
    kputs("joist: memory: ");
    kputu(usable / (1024ull * 1024ull));
    kputs(" MiB usable in ");
    kputu(ranges);
    kputs(" ranges, ");
    kputu(mm->entry_count);
    kputs(" map entries\n");
    kputs("joist: kernel at ");
    kputx(kaddr_req.response->virtual_base);
    kputs(" (physical ");
    kputx(kaddr_req.response->physical_base);
    kputs("), direct map at ");
    kputx(hhdm_req.response->offset);
    kputs("\n");
    frames_init(mm, hhdm_req.response->offset);
    frames_selfcheck(hhdm_req.response->offset);
    kputs("joist: frames: ");
    kputu(frames.nfree);
    kputs(" free of ");
    kputu(frames.nframes);
    kputs(" covered (");
    kputu(frames.nfree * FRAME_SIZE / (1024ull * 1024ull));
    kputs(" MiB)\n");
    const char *perr =
        paging_init(&frames, mm, hhdm_req.response->offset, kaddr_req.response->virtual_base,
                    kaddr_req.response->physical_base);
    if (perr != NULL) {
        panic(perr);
    }
    perr = gdt_guard_ist();
    if (perr != NULL) {
        panic(perr);
    }
    kputs("joist: page tables: the kernel's own, loaded (");
    kputu(frames.nfree);
    kputs(" frames free after)\n");
    const char *aerr = apic_init(hhdm_req.response->offset);
    if (aerr != NULL) {
        panic(aerr);
    }
    __asm__ volatile("sti");
    /* Sleep until 20 ticks (200 ms) have arrived; the bound only matters if
     * the timer never fires. */
    for (uint32_t spin = 0; apic_ticks() < 20u && spin < 1000000u; spin++) {
        __asm__ volatile("hlt");
    }
    __asm__ volatile("cli");
    if (apic_ticks() < 20u) {
        panic("the APIC timer didn't tick");
    }
    kputs("joist: timer: ");
    kputu(apic_ticks());
    kputs(" ticks at ");
    kputu(APIC_TICK_HZ);
    kputs(" Hz, ");
    kputu(apic_counts_per_tick());
    kputs(" LAPIC counts per tick\n");
    /* The kernel's clock is the TSC's (apic_clock_ns); where it isn't
     * invariant it may stand still or drift, and the timer's interrupts
     * keep the clock reaching its deadlines (run.c's timer_fired). */
    kputs(apic_tsc_invariant() ? "joist: clock: the TSC, invariant\n"
                               : "joist: clock: the TSC, not said to be invariant: deadlines kept by the timer's interrupts\n");
    kputs("joist: modules: ");
    kputu(module_req.response != NULL ? module_req.response->module_count : 0);
    kputs("\n");
    if (mp_req.response != NULL) {
        kputs("joist: cpus: ");
        kputu(mp_req.response->cpu_count);
        kputs(" found, bsp lapic id ");
        kputu(mp_req.response->bsp_lapic_id);
        kputs("\n");
    } else {
        kputs("joist: cpus: no MP response; 1 in use\n");
    }

#if defined(JOIST_SELFTEST) && JOIST_SELFTEST == 1
    kputs("joist: selftest: ud2\n");
    __asm__ volatile("ud2");
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST == 2
    kputs("joist: selftest: page fault\n");
    (void)*(volatile uint64_t *)0xffff900000000000ull;
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST == 3
    kputs("joist: selftest: int3\n");
    __asm__ volatile("int3");
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST == 4
    kputs("joist: selftest: write to read-only data\n");
    *(volatile char *)(uintptr_t)selftest_rodata = 'x';
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST == 6
    /* Only meaningful in the UBSan build (selftest.sh runs it there). */
    kputs("joist: selftest: signed overflow\n");
    volatile int64_t big = INT64_MAX;
    volatile int64_t one = 1;
    big = big + one;
    (void)big;
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST == 7
    /* A page fault that can't be delivered (the stack itself is unmapped)
     * becomes a double fault, which must arrive on its own stack (IST1). */
    kputs("joist: selftest: double fault\n");
    __asm__ volatile("movq $0xffff900000000000, %%rsp\n\tpushq %%rax" : : : "memory");
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST == 8
    run_selftest_stack_guard();
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST == 9
    kputs("joist: selftest: stack smash\n");
    smash(64);
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST >= 10 && JOIST_SELFTEST <= 12
    /* A push just under an IST stack's bottom lands in its guard page: the
     * page fault can't be delivered there either, so it becomes the double
     * fault (which arrives at the top of its own stack). Without the guard
     * the push would succeed and the run go on. 10: the double fault's
     * stack, 11: NMI's, 12: machine check's (CPU 0's). */
    kputs("joist: selftest: push into the guard under IST stack ");
    kputu(JOIST_SELFTEST - 9);
    kputs("\n");
    __asm__ volatile("movq %0, %%rsp\n\tpushq %%rax"
                     :
                     : "r"(gdt_ist_bottom(0, JOIST_SELFTEST - 9))
                     : "memory");
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST == 13
    /* An NMI (int $2 goes through the same gate) arrives on its own stack. */
    kputs("joist: selftest: nmi\n");
    __asm__ volatile("int $2");
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST == 14
    kputs("joist: selftest: machine check\n");
    __asm__ volatile("int $18");
#elif defined(JOIST_SELFTEST) && JOIST_SELFTEST == 5
    kputs("joist: selftest: execute data\n");
    selftest_code[0] = 0xc3; /* ret */
    ((void (*)(void))(uintptr_t)selftest_code)();
#endif
    /* The root task: the first module (limine.conf loads hello.elf). Selftest
     * builds stop before it; the normal boot ends when it exits. */
#if !defined(JOIST_SELFTEST)
    if (module_req.response == NULL || module_req.response->module_count == 0) {
        panic("no root task module");
    }
    panic(run_start(&frames, hhdm_req.response->offset, mm, module_req.response, mp_req.response));
#endif
    kputs("joist: ok\n");
    kernel_finish(0);
}
