/* SPDX-License-Identifier: GPL-3.0-only */
/* elf_promises.h - everything elf.h promises about an image elf_validate
 * accepted, checked independently of it. Shared by the property test and the
 * libFuzzer target. */
#ifndef JOIST_TEST_ELF_PROMISES_H
#define JOIST_TEST_ELF_PROMISES_H

#include "elf.h"

#include <inttypes.h>
#include <stdio.h>

static inline int elf_promises_hold(const struct elf_image *im, size_t size, uint64_t ulo,
                                    uint64_t uhi, char *why, size_t len)
{
    int entry_ok = 0;
    if (im->nsegments == 0 || im->nsegments > ELF_MAX_SEGMENTS) {
        (void)snprintf(why, len, "%u segments", im->nsegments);
        return 0;
    }
    for (uint32_t i = 0; i < im->nsegments; i++) {
        const struct elf_segment *s = &im->seg[i];
        if (s->memsz == 0 || s->filesz > s->memsz || s->offset > size ||
            s->filesz > size - s->offset || s->vaddr < ulo || s->vaddr > uhi ||
            s->memsz > uhi - s->vaddr || s->vaddr % 4096 != s->offset % 4096 ||
            ((s->flags & ELF_SEG_W) && (s->flags & ELF_SEG_X))) {
            (void)snprintf(why, len, "segment %u breaks a promise", i);
            return 0;
        }
        if (i != 0) {
            const struct elf_segment *p = &im->seg[i - 1];
            uint64_t pend = (p->vaddr + p->memsz + 4095u) & ~(uint64_t)4095u;
            if ((s->vaddr & ~(uint64_t)4095u) < pend) {
                (void)snprintf(why, len, "segments %u and %u overlap or are unsorted", i - 1, i);
                return 0;
            }
        }
        entry_ok |=
            (s->flags & ELF_SEG_X) && im->entry >= s->vaddr && im->entry - s->vaddr < s->memsz;
    }
    if (!entry_ok) {
        (void)snprintf(why, len, "entry %" PRIx64 " not in an executable segment", im->entry);
        return 0;
    }
    return 1;
}

#endif /* JOIST_TEST_ELF_PROMISES_H */
