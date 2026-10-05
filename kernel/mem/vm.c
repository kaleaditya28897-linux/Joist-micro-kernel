/* vm.c - x86_64 four-level page tables. See vm.h. */
#include "vm.h"

#define PTE_P    (1ull << 0)
#define PTE_W    (1ull << 1)
#define PTE_U    (1ull << 2)
#define PTE_PWT  (1ull << 3)
#define PTE_PCD  (1ull << 4)
#define PTE_PS   (1ull << 7) /* large page (in a PD entry: 2 MiB) */
#define PTE_G    (1ull << 8)
#define PTE_NX   (1ull << 63)
#define PTE_ADDR 0x000ffffffffff000ull
#define PA_MAX   0x000fffffffffffffull /* 52-bit physical addresses */

static int canonical(uint64_t va)
{
    uint64_t top = va >> 47; /* bits 63..47 all equal */
    return top == 0 || top == 0x1ffffu;
}

static unsigned idx(uint64_t va, unsigned level) /* level 3 = PML4 ... 0 = PT */
{
    return (unsigned)((va >> (12u + 9u * level)) & 0x1ffu);
}

static uint64_t *table(const struct vm_space *s, uint64_t phys)
{
    return (uint64_t *)s->phys_to_ptr(s->ctx, phys);
}

kerr vm_create(struct vm_space *s, kerr (*get_frame)(void *, uint64_t *),
               void *(*phys_to_ptr)(void *, uint64_t), void *ctx)
{
    if (s == NULL || get_frame == NULL || phys_to_ptr == NULL) {
        return KERR_INVAL;
    }
    s->get_frame = get_frame;
    s->phys_to_ptr = phys_to_ptr;
    s->ctx = ctx;
    return get_frame(ctx, &s->root);
}

/* The entry for va at `level` (2 = PD for a large page, 0 = PT), creating
 * the intermediate tables on the way. Table entries are created present,
 * writable and user-accessible: the leaf decides, since x86 ANDs the W and U
 * bits and ORs NX down the walk. KERR_STATE if a large page is in the way. */
static kerr walk_create(struct vm_space *s, uint64_t va, unsigned level, uint64_t **entry)
{
    uint64_t *t = table(s, s->root);
    for (unsigned l = 3; l > level; l--) {
        uint64_t *e = &t[idx(va, l)];
        if (*e & PTE_P) {
            if (*e & PTE_PS) {
                return KERR_STATE; /* a large page covers this range */
            }
        } else {
            uint64_t f;
            kerr k = s->get_frame(s->ctx, &f);
            if (k != KERR_OK) {
                return k;
            }
            *e = (f & PTE_ADDR) | PTE_P | PTE_W | PTE_U;
        }
        t = table(s, *e & PTE_ADDR);
    }
    *entry = &t[idx(va, level)];
    return KERR_OK;
}

kerr vm_map(struct vm_space *s, uint64_t va, uint64_t pa, uint64_t len, uint64_t page_size,
            unsigned perms)
{
    if (page_size != VM_PAGE && page_size != VM_LARGE) {
        return KERR_INVAL;
    }
    if (len == 0 || va % page_size != 0 || pa % page_size != 0 || len % page_size != 0 ||
        (perms & VM_W && perms & VM_X) || len - 1u > PA_MAX || pa > PA_MAX - (len - 1u) ||
        va > UINT64_MAX - (len - 1u) || !canonical(va) || !canonical(va + (len - 1u))) {
        return KERR_INVAL;
    }
    /* One half of the address space only: no wrapping across the hole. */
    if ((va >> 47) != ((va + (len - 1u)) >> 47)) {
        return KERR_INVAL;
    }
    uint64_t leaf = PTE_P | ((perms & VM_W) ? PTE_W : 0) | ((perms & VM_USER) ? PTE_U : 0) |
                    ((perms & VM_GLOBAL) ? PTE_G : 0) | ((perms & VM_X) ? 0 : PTE_NX) |
                    ((perms & VM_UNCACHED) ? (PTE_PCD | PTE_PWT) : 0) |
                    (page_size == VM_LARGE ? PTE_PS : 0);
    unsigned level = page_size == VM_LARGE ? 1u : 0u;
    /* Check every page first, so a refused call changes no mappings (tables
     * it created on the way stay, empty, which is harmless). */
    for (uint64_t off = 0; off < len; off += page_size) {
        uint64_t *e;
        kerr k = walk_create(s, va + off, level, &e);
        if (k != KERR_OK) {
            return k;
        }
        if (*e & PTE_P) {
            return KERR_STATE; /* already mapped, or a table where a large page would go */
        }
    }
    for (uint64_t off = 0; off < len; off += page_size) {
        uint64_t *e;
        /* The first pass created every table, so this can't fail; if it ever
         * did, report it rather than write through an unset pointer. */
        if (walk_create(s, va + off, level, &e) != KERR_OK) {
            return KERR_STATE;
        }
        *e = ((pa + off) & PTE_ADDR) | leaf;
    }
    return KERR_OK;
}

kerr vm_translate(const struct vm_space *s, uint64_t va, uint64_t *pa, unsigned *perms)
{
    if (!canonical(va)) {
        return KERR_RANGE;
    }
    uint64_t *t = table(s, s->root);
    int w = 1;
    int u = 1;
    int x = 1;
    for (unsigned l = 3;; l--) {
        uint64_t e = t[idx(va, l)];
        if (!(e & PTE_P)) {
            return KERR_RANGE;
        }
        w &= (e & PTE_W) != 0;
        u &= (e & PTE_U) != 0;
        x &= (e & PTE_NX) == 0;
        if (l == 0 || (l == 1 && (e & PTE_PS))) {
            uint64_t span = l == 0 ? VM_PAGE : VM_LARGE;
            *pa = (e & PTE_ADDR & ~(span - 1u)) | (va & (span - 1u));
            *perms = (w ? VM_W : 0u) | (x ? VM_X : 0u) | (u ? VM_USER : 0u) |
                     ((e & PTE_G) ? VM_GLOBAL : 0u) |
                     ((e & PTE_PCD) && (e & PTE_PWT) ? VM_UNCACHED : 0u);
            return KERR_OK;
        }
        if (e & PTE_PS) {
            return KERR_RANGE; /* 1 GiB pages are never created here */
        }
        t = table(s, e & PTE_ADDR);
    }
}
