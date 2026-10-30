/* SPDX-License-Identifier: GPL-3.0-only */
/* paging.c - the kernel's own page tables (Phase 2 step 4).
 *
 * Built with mem/vm.c from boot frames, then loaded: the kernel image with
 * exactly its linked permissions (requests R+W, code R+X, read-only data R,
 * data and BSS R+W), and a direct map of RAM (usable, bootloader-reclaimable,
 * ACPI, kernel and modules) at Limine's direct-map offset, writable, never
 * executable. Reserved regions, which may be device memory, are not mapped.
 * Everything is global (the kernel half is the same in every space). */
#include "paging.h"

#include "joist/kprint.h"

extern uint8_t joist_image_start[], joist_text_start[], joist_text_end[], joist_rodata_start[],
    joist_rodata_end[], joist_data_start[], joist_data_end[];

struct boot_frames {
    struct frame_alloc *fa;
    uint64_t hhdm;
};
static struct boot_frames bf;
static struct vm_space kspace;

static kerr get_zeroed_frame(void *ctx, uint64_t *phys)
{
    struct boot_frames *b = ctx;
    kerr k = frame_get(b->fa, phys);
    if (k == KERR_OK) {
        volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)(b->hhdm + *phys);
        for (unsigned i = 0; i < 512u; i++) {
            p[i] = 0;
        }
    }
    return k;
}

static void *phys_to_ptr(void *ctx, uint64_t phys)
{
    const struct boot_frames *b = ctx;
    return (void *)(uintptr_t)(b->hhdm + phys);
}

/* Map [base, base + len) at hhdm + base: 2 MiB pages for the aligned middle,
 * 4 KiB at the edges; rounded inwards to whole pages, never outwards, so
 * neighbouring regions of other types are never touched. */
static kerr map_direct(uint64_t hhdm, uint64_t base, uint64_t len)
{
    uint64_t a = (base + VM_PAGE - 1u) & ~(VM_PAGE - 1u);
    uint64_t e = (base + len) & ~(VM_PAGE - 1u);
    while (a < e) {
        uint64_t size = VM_PAGE;
        if (a % VM_LARGE == 0 && e - a >= VM_LARGE) {
            size = VM_LARGE;
        }
        uint64_t n;
        if (size == VM_PAGE) { /* run of 4 KiB pages up to the next 2 MiB boundary */
            uint64_t next = (a + VM_LARGE) & ~(VM_LARGE - 1u);
            n = (next < e ? next : e) - a;
        } else {
            n = (e - a) & ~(VM_LARGE - 1u);
        }
        kerr k = vm_map(&kspace, hhdm + a, a, n, size, VM_W | VM_GLOBAL);
        if (k != KERR_OK) {
            return k;
        }
        a += n;
    }
    return KERR_OK;
}

static kerr map_image(uint64_t vbase, uint64_t pbase, uint8_t *from, uint8_t *to, unsigned perms)
{
    uint64_t v = (uint64_t)(uintptr_t)from;
    uint64_t end = ((uint64_t)(uintptr_t)to + VM_PAGE - 1u) & ~(VM_PAGE - 1u);
    if (end == v) {
        return KERR_OK;
    }
    return vm_map(&kspace, v, pbase + (v - vbase), end - v, VM_PAGE, perms | VM_GLOBAL);
}

const char *paging_init(struct frame_alloc *fa, const struct limine_memmap_response *mm,
                        uint64_t hhdm, uint64_t kvirt, uint64_t kphys)
{
    bf = (struct boot_frames){.fa = fa, .hhdm = hhdm};
    if (vm_create(&kspace, get_zeroed_frame, phys_to_ptr, &bf) != KERR_OK) {
        return "no frame for the PML4";
    }
    if (map_image(kvirt, kphys, joist_image_start, joist_text_start, VM_W) != KERR_OK ||
        map_image(kvirt, kphys, joist_text_start, joist_text_end, VM_X) != KERR_OK ||
        map_image(kvirt, kphys, joist_rodata_start, joist_rodata_end, 0) != KERR_OK ||
        map_image(kvirt, kphys, joist_data_start, joist_data_end, VM_W) != KERR_OK) {
        return "mapping the kernel image failed";
    }
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        const struct limine_memmap_entry *m = mm->entries[i];
        if (m->type == LIMINE_MEMMAP_USABLE || m->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            m->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE || m->type == LIMINE_MEMMAP_ACPI_NVS ||
            m->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            if (map_direct(hhdm, m->base, m->length) != KERR_OK) {
                return "mapping the direct map failed";
            }
        }
    }
    /* NX must be enabled before tables with the NX bit are live, or it is a
     * reserved bit and every access faults. Then write protection in ring 0
     * and global pages. */
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000080u));
    lo |= 1u << 11; /* EFER.NXE */
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xc0000080u));
    uint64_t cr0;
    uint64_t cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 | (1ull << 16)) : "memory"); /* WP */
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | (1ull << 7)) : "memory"); /* PGE */
    /* Every upper-half PML4 entry gets its table now, so every task's PML4,
     * copying them, shares the whole kernel half for good (vm_share_upper). */
    if (vm_prefill_upper(&kspace) != KERR_OK) {
        return "no frames for the kernel half's tables";
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(kspace.root) : "memory");
    return NULL;
}

const struct vm_space *paging_kernel_space(void)
{
    return &kspace;
}

kerr paging_map_device(uint64_t va, uint64_t pa, uint64_t len)
{
    kerr k = vm_map(&kspace, va, pa, len, VM_PAGE, VM_W | VM_GLOBAL | VM_UNCACHED);
    if (k == KERR_OK) {
        for (uint64_t off = 0; off < len; off += VM_PAGE) {
            __asm__ volatile("invlpg (%0)" : : "r"(va + off) : "memory");
        }
    }
    return k;
}

void paging_init_ap(void)
{
    /* What paging_init set on the first CPU, in the same order: NX, write
     * protection in ring 0, global pages; then the kernel's own tables. */
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000080u));
    lo |= 1u << 11;
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xc0000080u));
    uint64_t cr0;
    uint64_t cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0 | (1ull << 16)) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | (1ull << 7)) : "memory");
    __asm__ volatile("mov %0, %%cr3" : : "r"(kspace.root) : "memory");
}

kerr paging_guard(uint64_t va)
{
    /* The page goes from the kernel's tables, so any access to it faults;
     * those tables are every task's upper half too (vm_share_upper). */
    kerr e = vm_unmap(&kspace, va, VM_PAGE);
    if (e == KERR_OK) {
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
    return e;
}

