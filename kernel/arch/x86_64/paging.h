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

#endif /* JOIST_ARCH_PAGING_H */
