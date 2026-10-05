/* elf.h - validating the root task's ELF64 image (Phase 2, docs/phase2-plan.md
 * step 6).
 *
 * The root task arrives as a boot module: bytes the kernel must not trust.
 * elf_validate checks the whole image before anything is mapped and produces
 * a small, fixed-size description the loader then follows. It reads fields as
 * little-endian bytes (no pointer casts, so alignment doesn't matter), never
 * reads outside [file, file + size), and is bounded by ELF_MAX_PHDRS.
 *
 * Accepted: a static x86_64 executable (ET_EXEC, EM_X86_64, ELFCLASS64,
 * little-endian), whose loadable segments lie in [user_lo, user_hi), are
 * sorted and don't overlap (page-granular), have offset and address congruent
 * modulo 4 KiB, file size <= memory size, and are never both writable and
 * executable (W^X); with its entry point in an executable segment. Refused:
 * dynamic linking (PT_DYNAMIC, PT_INTERP), TLS (PT_TLS; not before Phase 7),
 * and anything malformed. Other segment types (notes, GNU_STACK, ...) are
 * ignored. Compiles freestanding and on the host (§8.1). */
#ifndef JOIST_ELF_H
#define JOIST_ELF_H

#include "joist/kerr.h"

#include <stddef.h>
#include <stdint.h>

#define ELF_MAX_PHDRS    16u /* more program headers than this is refused */
#define ELF_MAX_SEGMENTS 8u  /* loadable segments */

#define ELF_SEG_X 1u
#define ELF_SEG_W 2u
#define ELF_SEG_R 4u

struct elf_segment {
    uint64_t vaddr;  /* where it goes in the task */
    uint64_t memsz;  /* bytes in memory; the part past filesz is zeroed */
    uint64_t offset; /* where its bytes start in the file */
    uint64_t filesz; /* bytes taken from the file */
    uint32_t flags;  /* ELF_SEG_R, _W, _X */
};

struct elf_image {
    uint64_t entry;
    uint32_t nsegments;
    struct elf_segment seg[ELF_MAX_SEGMENTS];
};

/* Validate file[0, size) as described above; on success fill *out. Any
 * failure is KERR_INVAL and leaves *out unspecified. */
kerr elf_validate(const uint8_t *file, size_t size, uint64_t user_lo, uint64_t user_hi,
                  struct elf_image *out);

#endif /* JOIST_ELF_H */
