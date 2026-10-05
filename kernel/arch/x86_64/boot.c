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
#include "user.h"
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
__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t req_end[2] =
    LIMINE_REQUESTS_END_MARKER;

/* The kernel's own stack (16 KiB, in BSS), for the TSS's RSP0 once user
 * mode exists (step 2); until then the kernel runs on Limine's. */
__attribute__((aligned(16))) uint8_t kernel_stack[16384];

/* QEMU's isa-debug-exit device (iobase 0xf4): QEMU exits with status
 * (code << 1) | 1, so `make qemu-test` sees the kernel's verdict. On hardware
 * the port is unused and this just halts. */
__attribute__((noreturn)) void kernel_finish(uint8_t code)
{
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

void kmain(void)
{
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
    kputu(usable / (1024u * 1024u));
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
    kputu(frames.nfree * FRAME_SIZE / (1024u * 1024u));
    kputs(" MiB)\n");
    const char *perr =
        paging_init(&frames, mm, hhdm_req.response->offset, kaddr_req.response->virtual_base,
                    kaddr_req.response->physical_base);
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
    kputs("joist: modules: ");
    kputu(module_req.response != NULL ? module_req.response->module_count : 0);
    kputs("\n");

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
    const struct limine_file *root = module_req.response->modules[0];
    panic(user_run_root(&frames, hhdm_req.response->offset, (const uint8_t *)root->address,
                        root->size));
#endif
    kputs("joist: ok\n");
    kernel_finish(0);
}
