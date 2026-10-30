/* SPDX-License-Identifier: GPL-3.0-only */
/* frame.h - the boot-time physical frame allocator (Phase 2, docs/phase2-plan.md
 * step 3).
 *
 * Two bits per 4 KiB frame, over the span from the lowest to the highest
 * usable frame the bootloader reported: `taken` (not available now) and
 * `fixed` (never available: a gap between usable ranges, or reserved). The
 * second is what lets frame_put refuse to "free" a frame that was never
 * handed out, such as the kernel image.
 *
 * The bitmaps' storage is given by the caller (the boot code carves it out of
 * a usable range through the direct map and then reserves it here), so the
 * allocator itself never allocates.
 *
 * Boot-time only (§5.1): after boot, kernel objects are paid for from memory
 * donated by tasks (0003, 0004), which replaces this allocator's callers.
 *
 * Bounds (§5.1): every operation is bounded by the size of the bitmap, which
 * is fixed at boot by the machine's memory, never by anything a task controls.
 * Compiles freestanding and on the host (§8.1). */
#ifndef JOIST_MEM_FRAME_H
#define JOIST_MEM_FRAME_H

#include "joist/kerr.h"

#include <stddef.h>
#include <stdint.h>

#define FRAME_SIZE 4096u

/* A range of usable physical memory, as the bootloader reports it. The
 * allocator trims each to whole frames and rejects malformed maps. */
struct frame_range {
    uint64_t base;
    uint64_t length;
};

struct frame_alloc {
    uint64_t *taken;   /* one bit per frame from base_pfn; 1 = not available now */
    uint64_t *fixed;   /* 1 = never available (gap, reserved, past the end) */
    uint64_t base_pfn; /* the first frame the bitmaps cover */
    uint64_t nframes;  /* frames covered */
    uint64_t nfree;    /* frames available now */
    uint64_t hint;     /* word index to start searching from */
};

/* Bytes of storage frame_init needs for this map (both bitmaps, each rounded
 * up to whole 64-bit words), or 0 if the map is malformed or empty. */
size_t frame_bitmap_bytes(const struct frame_range *usable, size_t n);

/* Set up over the usable ranges. The ranges must be sorted by base and must
 * not overlap; each is trimmed inwards to whole frames, and ranges smaller
 * than a frame after trimming are ignored. storage must be 8-byte aligned and
 * at least frame_bitmap_bytes() long. KERR_INVAL for a malformed map, an
 * empty one, or too little or misaligned storage; nothing is set up then. */
kerr frame_init(struct frame_alloc *fa, const struct frame_range *usable, size_t n, void *storage,
                size_t storage_bytes);

/* Mark [phys, phys + len) permanently unavailable, rounded outwards to whole
 * frames (the kernel image, the bitmaps' own storage, boot modules). Parts
 * outside the covered span are ignored. Meant for boot, before frames are
 * handed out: a reserved frame can never be put back. */
void frame_reserve(struct frame_alloc *fa, uint64_t phys, uint64_t len);

/* Take one free frame; its physical address in *phys. KERR_NOMEM if none. */
kerr frame_get(struct frame_alloc *fa, uint64_t *phys);

/* Take the frames [phys, phys + len) for good (they leave the allocator
 * like reserved ones), but only if every one of them is free now: KERR_STATE
 * (and nothing taken) if any is taken or never allocatable, KERR_RANGE if
 * the range isn't wholly covered, KERR_INVAL if unaligned or empty. How the
 * kernel carves out the root task's memory pool (0013), contiguous, at boot.
 * Bounded by the range's frames. */
kerr frame_take_range(struct frame_alloc *fa, uint64_t phys, uint64_t len);

/* Give a frame back. KERR_INVAL if phys isn't frame-aligned, KERR_RANGE if
 * the bitmaps don't cover it, KERR_STATE if it is already free or was never
 * allocatable (a double free, or freeing the kernel image, is a kernel bug,
 * reported rather than ignored). */
kerr frame_put(struct frame_alloc *fa, uint64_t phys);

/* Self-check for tests: nfree equals the frames not taken, every fixed
 * frame is taken, and every bit past nframes is set in both. 1 if so. */
int frame_check(const struct frame_alloc *fa);

#endif /* JOIST_MEM_FRAME_H */
