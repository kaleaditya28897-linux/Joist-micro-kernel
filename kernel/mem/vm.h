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
#define VM_W        1u /* writable */
#define VM_X        2u /* executable (otherwise the NX bit is set) */
#define VM_USER     4u /* user-accessible */
#define VM_GLOBAL   8u
#define VM_UNCACHED 16u /* device memory: cache disabled, write-through */

struct vm_space {
    uint64_t root; /* physical address of the PML4 */
    /* A zeroed frame for a new table, in *phys; KERR_NOMEM if none. */
    kerr (*get_frame)(void *ctx, uint64_t *phys);
    /* Where the code can reach physical address phys. */
    void *(*phys_to_ptr)(void *ctx, uint64_t phys);
    void *ctx;
    /* Where tables that become empty go (vm_unmap, vm_destroy); NULL keeps
     * them. Set after vm_create. */
    void (*put_frame)(void *ctx, uint64_t phys);
};

/* A new, empty address space (allocates the PML4). */
kerr vm_create(struct vm_space *s, kerr (*get_frame)(void *, uint64_t *),
               void *(*phys_to_ptr)(void *, uint64_t), void *ctx);

/* Map [va, va + len) to [pa, pa + len) with 4 KiB pages (page_size VM_PAGE)
 * or 2 MiB pages (VM_LARGE); va, pa and len aligned to it. */
kerr vm_map(struct vm_space *s, uint64_t va, uint64_t pa, uint64_t len, uint64_t page_size,
            unsigned perms);

/* Remove the 4 KiB mappings of [va, va + len), all of which must exist
 * (KERR_RANGE otherwise, changing nothing; KERR_INVAL for an unaligned or
 * non-canonical range). Tables of the lower (user) half that become empty
 * are freed through put_frame, bottom up; the upper half's never are, since
 * every space shares them. Flushing the TLB is the caller's job. Bounded:
 * per page, four levels and, per emptied table, one scan of its 512 entries. */
kerr vm_unmap(struct vm_space *s, uint64_t va, uint64_t len);

/* Free every table of the lower half, then the PML4, through put_frame
 * (the mapped frames themselves are not the tables' to free). The space is
 * unusable afterwards. Bounded by the tables present times 512. */
void vm_destroy(struct vm_space *s);

/* Give every upper-half PML4 entry (256-511) a table, so that copying the
 * entries into another space shares the whole kernel half for good: later
 * kernel mappings land in these tables, never in the PML4, and so appear in
 * every space (Phase 3 plan, choice 4). KERR_NOMEM if frames run out. */
kerr vm_prefill_upper(struct vm_space *s);

/* Copy src's upper-half PML4 entries into dst (a new, empty space). */
void vm_share_upper(struct vm_space *dst, const struct vm_space *src);

/* ---- tables made by someone else (0020, 0021) ----
 * For a user space whose tables are kept by the core in pages lent to it:
 * the core decides where each table goes and when it goes, and these only
 * write the entries. Lower half only; levels 2 (PDPT), 1 (PD), 0 (PT). */

/* Make the zeroed page at phys va's table at `level`: its entry in the
 * table above, which must be absent, every table above it present
 * (KERR_STATE otherwise; KERR_INVAL for the upper half or a bad level). */
kerr vm_table_set(struct vm_space *s, uint64_t va, unsigned level, uint64_t phys);

/* Clear the entry naming va's table at `level`; the table's page is the
 * caller's. KERR_STATE if there is none. */
kerr vm_table_clear(struct vm_space *s, uint64_t va, unsigned level);

/* Overwrite the 4 KiB entries of [va, va + len), every one present
 * (KERR_RANGE otherwise, changing nothing), with [pa, pa + len) and perms,
 * as vm_map would write them. */
kerr vm_remap(struct vm_space *s, uint64_t va, uint64_t pa, uint64_t len, unsigned perms);

/* Give the 4 KiB entries of [va, va + len), every one present (KERR_RANGE
 * otherwise, changing nothing), the permissions perms; the pages stay. */
kerr vm_protect(struct vm_space *s, uint64_t va, uint64_t len, unsigned perms);

/* Forget the lower half without freeing its tables (they are not this
 * code's to free), then free the PML4 through put_frame. */
void vm_release(struct vm_space *s);

/* Software walk: the physical address va maps to and its permissions.
 * KERR_RANGE if va isn't mapped. */
kerr vm_translate(const struct vm_space *s, uint64_t va, uint64_t *pa, unsigned *perms);

#endif /* JOIST_MEM_VM_H */
