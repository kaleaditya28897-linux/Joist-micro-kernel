/* SPDX-License-Identifier: GPL-3.0-only */
/* frame.c - the boot-time physical frame allocator. See frame.h. */
#include "frame.h"

#define WORD_BITS 64u

static uint64_t up(uint64_t x)
{
    return (x + FRAME_SIZE - 1u) / FRAME_SIZE;
}

static uint64_t down(uint64_t x)
{
    return x / FRAME_SIZE;
}

/* Validate the map and find the span of whole usable frames [*lo, *hi). */
static kerr span(const struct frame_range *usable, size_t n, uint64_t *lo, uint64_t *hi)
{
    uint64_t prev_end = 0;
    int any = 0;
    if (usable == NULL || n == 0) {
        return KERR_INVAL;
    }
    for (size_t i = 0; i < n; i++) {
        uint64_t b = usable[i].base;
        uint64_t l = usable[i].length;
        if (l == 0 || b > UINT64_MAX - l) {
            return KERR_INVAL; /* empty, or wraps the address space */
        }
        if (i != 0 && b < prev_end) {
            return KERR_INVAL; /* unsorted or overlapping */
        }
        prev_end = b + l;
        uint64_t f0 = up(b);
        uint64_t f1 = down(b + l);
        if (f0 < f1) {
            if (!any) {
                *lo = f0;
                any = 1;
            }
            *hi = f1;
        }
    }
    return any ? KERR_OK : KERR_INVAL;
}

size_t frame_bitmap_bytes(const struct frame_range *usable, size_t n)
{
    uint64_t lo = 0;
    uint64_t hi = 0;
    if (span(usable, n, &lo, &hi) != KERR_OK) {
        return 0;
    }
    uint64_t words = (hi - lo + WORD_BITS - 1u) / WORD_BITS;
    if (words > SIZE_MAX / (2u * sizeof(uint64_t))) {
        return 0;
    }
    return (size_t)(2u * words * sizeof(uint64_t));
}

static void set_bit(uint64_t *map, uint64_t i)
{
    map[i / WORD_BITS] |= (uint64_t)1u << (i % WORD_BITS);
}

static void clear_bit(uint64_t *map, uint64_t i)
{
    map[i / WORD_BITS] &= ~((uint64_t)1u << (i % WORD_BITS));
}

static int bit(const uint64_t *map, uint64_t i)
{
    return (int)((map[i / WORD_BITS] >> (i % WORD_BITS)) & 1u);
}

kerr frame_init(struct frame_alloc *fa, const struct frame_range *usable, size_t n, void *storage,
                size_t storage_bytes)
{
    uint64_t lo = 0;
    uint64_t hi = 0;
    if (fa == NULL || storage == NULL || ((uintptr_t)storage % sizeof(uint64_t)) != 0) {
        return KERR_INVAL;
    }
    if (span(usable, n, &lo, &hi) != KERR_OK) {
        return KERR_INVAL;
    }
    size_t need = frame_bitmap_bytes(usable, n);
    if (need == 0 || storage_bytes < need) {
        return KERR_INVAL;
    }
    size_t words = need / (2u * sizeof(uint64_t));
    fa->taken = storage;
    fa->fixed = fa->taken + words;
    fa->base_pfn = lo;
    fa->nframes = hi - lo;
    fa->nfree = 0;
    fa->hint = 0;
    /* Everything unavailable for good, then open up the usable frames: gaps
     * between ranges, and the tail of the last word, stay fixed. */
    for (size_t w = 0; w < words; w++) {
        fa->taken[w] = ~(uint64_t)0;
        fa->fixed[w] = ~(uint64_t)0;
    }
    for (size_t i = 0; i < n; i++) {
        uint64_t f0 = up(usable[i].base);
        uint64_t f1 = down(usable[i].base + usable[i].length);
        for (uint64_t f = f0; f < f1; f++) {
            clear_bit(fa->taken, f - lo);
            clear_bit(fa->fixed, f - lo);
            fa->nfree++;
        }
    }
    return KERR_OK;
}

void frame_reserve(struct frame_alloc *fa, uint64_t phys, uint64_t len)
{
    if (len == 0) {
        return;
    }
    uint64_t end = phys > UINT64_MAX - len ? UINT64_MAX : phys + len;
    uint64_t f0 = down(phys);
    uint64_t f1 = up(end);
    if (end == UINT64_MAX) {
        f1 = down(end) + 1u; /* up() would overflow */
    }
    uint64_t lo = fa->base_pfn;
    uint64_t hi = fa->base_pfn + fa->nframes;
    if (f0 < lo) {
        f0 = lo;
    }
    if (f1 > hi) {
        f1 = hi;
    }
    for (uint64_t f = f0; f < f1; f++) {
        if (!bit(fa->taken, f - lo)) {
            fa->nfree--;
        }
        set_bit(fa->taken, f - lo);
        set_bit(fa->fixed, f - lo);
    }
}

kerr frame_get(struct frame_alloc *fa, uint64_t *phys)
{
    uint64_t words = (fa->nframes + WORD_BITS - 1u) / WORD_BITS;
    if (fa->nfree == 0 || words == 0) {
        return KERR_NOMEM;
    }
    /* From the hint, wrapping once: bounded by the bitmap's size. */
    for (uint64_t n = 0; n < words; n++) {
        uint64_t w = (fa->hint + n) % words;
        uint64_t v = fa->taken[w];
        if (v != ~(uint64_t)0) {
            unsigned b = (unsigned)__builtin_ctzll(~v);
            uint64_t i = w * WORD_BITS + b;
            set_bit(fa->taken, i);
            fa->nfree--;
            fa->hint = w;
            *phys = (fa->base_pfn + i) * FRAME_SIZE;
            return KERR_OK;
        }
    }
    return KERR_NOMEM; /* nfree said otherwise: frame_check would fail */
}

kerr frame_take_range(struct frame_alloc *fa, uint64_t phys, uint64_t len)
{
    if (len == 0 || phys % FRAME_SIZE != 0 || len % FRAME_SIZE != 0 || phys > UINT64_MAX - len) {
        return KERR_INVAL;
    }
    uint64_t f0 = phys / FRAME_SIZE;
    uint64_t f1 = (phys + len) / FRAME_SIZE;
    if (f0 < fa->base_pfn || f1 > fa->base_pfn + fa->nframes) {
        return KERR_RANGE;
    }
    for (uint64_t f = f0; f < f1; f++) {
        if (bit(fa->taken, f - fa->base_pfn)) {
            return KERR_STATE; /* in use, or never allocatable: nothing taken */
        }
    }
    for (uint64_t f = f0; f < f1; f++) {
        set_bit(fa->taken, f - fa->base_pfn);
        set_bit(fa->fixed, f - fa->base_pfn); /* gone for good, like reserved */
        fa->nfree--;
    }
    return KERR_OK;
}

kerr frame_put(struct frame_alloc *fa, uint64_t phys)
{
    if (phys % FRAME_SIZE != 0) {
        return KERR_INVAL;
    }
    uint64_t f = phys / FRAME_SIZE;
    if (f < fa->base_pfn || f - fa->base_pfn >= fa->nframes) {
        return KERR_RANGE;
    }
    uint64_t i = f - fa->base_pfn;
    if (!bit(fa->taken, i) || bit(fa->fixed, i)) {
        return KERR_STATE; /* already free, or never allocatable */
    }
    clear_bit(fa->taken, i);
    fa->nfree++;
    return KERR_OK;
}

int frame_check(const struct frame_alloc *fa)
{
    uint64_t words = (fa->nframes + WORD_BITS - 1u) / WORD_BITS;
    uint64_t clear = 0;
    for (uint64_t i = 0; i < fa->nframes; i++) {
        clear += (uint64_t)(bit(fa->taken, i) == 0);
        if (bit(fa->fixed, i) && !bit(fa->taken, i)) {
            return 0; /* a never-allocatable frame looks available */
        }
    }
    for (uint64_t i = fa->nframes; i < words * WORD_BITS; i++) {
        if (!bit(fa->taken, i) || !bit(fa->fixed, i)) {
            return 0; /* a frame past the end looks available */
        }
    }
    return clear == fa->nfree && fa->hint <= words;
}
