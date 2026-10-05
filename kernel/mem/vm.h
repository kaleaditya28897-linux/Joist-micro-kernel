/* vm.h - x86_64 four-level page tables (Phase 2 step 4, docs/phase2-plan.md).
 *
 * Builds and inspects page tables without touching the CPU: tables are
 * reached only through two callbacks, one that hands out a zeroed frame and
 * one that turns a physical address into a pointer the code can use (the
 * kernel's direct map; on the host, a simulated physical memory). So all of
 * it is host-testable (§8.1); loading CR3 is arch/x86_64's job.
 *
 * Rules, enforced on every mapping (KERR_INVAL otherwise): addresses
 * canonical and aligned to the page size used; never writable and executable
 * at once (W^X); a page is mapped once (KERR_STATE on an overlap, at any
 * level). Bounded: one mapping call walks four levels per page it maps, and
 * the caller bounds the length. Compiles freestanding and on the host. */
#ifndef JOIST_MEM_VM_H
#define JOIST_MEM_VM_H

#include "joist/kerr.h"

#include <stddef.h>
#include <stdint.h>

#define VM_PAGE  0x1000ull
#define VM_LARGE 0x200000ull /* 2 MiB */

/* Mapping permissions (not PTE bits: vm.c translates). Readable is implied. */
#define VM_W      1u /* writable */
#define VM_X      2u /* executable (otherwise the NX bit is set) */
#define VM_USER   4u /* user-accessible */
#define VM_GLOBAL 8u

struct vm_space {
    uint64_t root; /* physical address of the PML4 */
    /* A zeroed frame for a new table, in *phys; KERR_NOMEM if none. */
    kerr (*get_frame)(void *ctx, uint64_t *phys);
    /* Where the code can reach physical address phys. */
    void *(*phys_to_ptr)(void *ctx, uint64_t phys);
    void *ctx;
};

/* A new, empty address space (allocates the PML4). */
kerr vm_create(struct vm_space *s, kerr (*get_frame)(void *, uint64_t *),
               void *(*phys_to_ptr)(void *, uint64_t), void *ctx);

/* Map [va, va + len) to [pa, pa + len) with 4 KiB pages (page_size VM_PAGE)
 * or 2 MiB pages (VM_LARGE); va, pa and len aligned to it. */
kerr vm_map(struct vm_space *s, uint64_t va, uint64_t pa, uint64_t len, uint64_t page_size,
            unsigned perms);

/* Software walk: the physical address va maps to and its permissions.
 * KERR_RANGE if va isn't mapped. */
kerr vm_translate(const struct vm_space *s, uint64_t va, uint64_t *pa, unsigned *perms);

#endif /* JOIST_MEM_VM_H */
