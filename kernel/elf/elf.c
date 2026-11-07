/* SPDX-License-Identifier: GPL-3.0-only */
/* elf.c - validating the root task's ELF64 image. See elf.h. */
#include "elf.h"

#define PAGE 4096u

/* ELF constants (System V gABI, x86_64 psABI). */
#define EHDR_SIZE  64u
#define PHDR_SIZE  56u
#define ET_EXEC    2u
#define EM_X86_64  62u
#define PT_LOAD    1u
#define PT_DYNAMIC 2u
#define PT_INTERP  3u
#define PT_TLS     7u
#define PF_X       1u
#define PF_W       2u
#define PF_R       4u

/* Little-endian reads; the caller has checked the bytes are inside the file. */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* Does [off, off + len) lie inside [0, size), without overflow? */
static int inside(uint64_t off, uint64_t len, uint64_t size)
{
    return off <= size && len <= size - off;
}

kerr elf_validate(const uint8_t *file, size_t size, uint64_t user_lo, uint64_t user_hi,
                  struct elf_image *out)
{
    if (file == NULL || out == NULL || size < EHDR_SIZE || user_lo >= user_hi) {
        return KERR_INVAL;
    }
    /* e_ident: magic, ELFCLASS64, ELFDATA2LSB, EV_CURRENT. */
    if (file[0] != 0x7fu || file[1] != 'E' || file[2] != 'L' || file[3] != 'F' || file[4] != 2u ||
        file[5] != 1u || file[6] != 1u) {
        return KERR_INVAL;
    }
    if (rd16(file + 16) != ET_EXEC || rd16(file + 18) != EM_X86_64 || rd32(file + 20) != 1u) {
        return KERR_INVAL;
    }
    uint64_t entry = rd64(file + 24);
    uint64_t phoff = rd64(file + 32);
    uint16_t phentsize = rd16(file + 54);
    uint16_t phnum = rd16(file + 56);
    if (phentsize != PHDR_SIZE || phnum == 0 || phnum > ELF_MAX_PHDRS ||
        !inside(phoff, (uint64_t)phnum * PHDR_SIZE, size)) {
        return KERR_INVAL;
    }

    out->entry = entry;
    out->nsegments = 0;
    int entry_ok = 0;
    uint64_t prev_end = 0; /* page-rounded end of the previous loadable segment */
    int tls = 0;          /* a TLS template seen, at (tls_off, tls_va, tls_mem) */
    uint64_t tls_off = 0, tls_va = 0, tls_mem = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = file + phoff + (uint64_t)i * PHDR_SIZE;
        uint32_t type = rd32(ph);
        uint32_t pflags = rd32(ph + 4);
        uint64_t offset = rd64(ph + 8);
        uint64_t vaddr = rd64(ph + 16);
        uint64_t filesz = rd64(ph + 32);
        uint64_t memsz = rd64(ph + 40);
        if (type == PT_DYNAMIC || type == PT_INTERP) {
            return KERR_INVAL; /* static executables only */
        }
        if (type == PT_TLS) {
            /* One, well formed; placed in a segment: checked below. */
            if (tls || filesz > memsz || !inside(offset, filesz, size) || vaddr > UINT64_MAX - memsz) {
                return KERR_INVAL;
            }
            tls = 1;
            tls_off = offset;
            tls_va = vaddr;
            tls_mem = memsz;
            continue;
        }
        if (type != PT_LOAD) {
            continue;
        }
        if (out->nsegments == ELF_MAX_SEGMENTS || memsz == 0 || filesz > memsz ||
            !inside(offset, filesz, size) || vaddr % PAGE != offset % PAGE) {
            return KERR_INVAL;
        }
        /* Inside the user range, without overflow, page-granular. */
        if (vaddr < user_lo || vaddr > user_hi || memsz > user_hi - vaddr) {
            return KERR_INVAL;
        }
        uint64_t start = vaddr - vaddr % PAGE;
        uint64_t end = vaddr + memsz;
        if (end % PAGE != 0) {
            if (end > UINT64_MAX - PAGE) {
                return KERR_INVAL;
            }
            end += PAGE - end % PAGE;
        }
        /* Sorted, and no two segments share a page. */
        if (out->nsegments != 0 && start < prev_end) {
            return KERR_INVAL;
        }
        prev_end = end;
        uint32_t f = ((pflags & PF_R) ? ELF_SEG_R : 0u) | ((pflags & PF_W) ? ELF_SEG_W : 0u) |
                     ((pflags & PF_X) ? ELF_SEG_X : 0u);
        if ((f & ELF_SEG_W) && (f & ELF_SEG_X)) {
            return KERR_INVAL; /* W^X */
        }
        if ((f & ELF_SEG_X) && entry >= vaddr && entry - vaddr < memsz) {
            entry_ok = 1;
        }
        out->seg[out->nsegments++] = (struct elf_segment){
            .vaddr = vaddr, .memsz = memsz, .offset = offset, .filesz = filesz, .flags = f};
    }
    if (out->nsegments == 0 || !entry_ok) {
        return KERR_INVAL;
    }
    if (tls) {
        /* Wholly in one loadable segment, at that segment's own place in
         * the file. */
        int placed = 0;
        for (uint32_t i = 0; i < out->nsegments && !placed; i++) {
            const struct elf_segment *s = &out->seg[i];
            placed = tls_va >= s->vaddr && tls_mem <= s->memsz && tls_va - s->vaddr <= s->memsz - tls_mem &&
                     tls_off >= s->offset && tls_off - s->offset == tls_va - s->vaddr;
        }
        if (!placed) {
            return KERR_INVAL;
        }
    }
    return KERR_OK;
}
