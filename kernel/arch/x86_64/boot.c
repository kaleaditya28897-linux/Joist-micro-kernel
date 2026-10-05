/* boot.c - the kernel's entry from Limine (Phase 2 step 1; decision 0015).
 *
 * Limine enters kmain in 64-bit long mode, higher half, interrupts off, with a
 * valid stack and its own GDT and page tables, no IDT (protocol base revision
 * 6, x86-64 machine state). What the kernel needs from Limine it asks for with
 * the requests below, which sit in their own section between Limine's start
 * and end markers; Limine fills in their response pointers before entry. */
#include "cpu.h"
#include "io.h"
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

static void panic(const char *why)
{
    kputs("joist: PANIC: ");
    kputs(why);
    kputs("\n");
    kernel_finish(1);
}

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
#endif
    kputs("joist: ok\n");
    kernel_finish(0);
}
