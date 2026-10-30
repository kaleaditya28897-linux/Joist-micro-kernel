/* SPDX-License-Identifier: GPL-3.0-only */
/* paging.h - the kernel's own page tables (Phase 2 step 4). */
#ifndef JOIST_ARCH_PAGING_H
#define JOIST_ARCH_PAGING_H

#include "frame.h"
#include "vm.h"

#include <limine/limine.h>
#include <stddef.h>

/* Build the kernel's tables and switch to them. NULL on success, otherwise
 * what failed (the caller panics: Limine's tables are still live then). */
const char *paging_init(struct frame_alloc *fa, const struct limine_memmap_response *mm,
                        uint64_t hhdm, uint64_t kvirt, uint64_t kphys);

const struct vm_space *paging_kernel_space(void);

/* Map device memory (uncached, writable, never executable) at va, after
 * paging_init. Then flushes the TLB entries for it. */
kerr paging_map_device(uint64_t va, uint64_t pa, uint64_t len);

/* The same control-register settings and the kernel's tables, on a CPU
 * after the first (Phase 3 step 9). */
void paging_init_ap(void);

/* Unmap one page of the kernel image (VM_PAGE-aligned): a guard page, under
 * a kernel stack (Phase 3's exit). */
kerr paging_guard(uint64_t va);

#endif /* JOIST_ARCH_PAGING_H */
