/* SPDX-License-Identifier: GPL-3.0-only */
/* cow.h - what cowstress (the root) and cowworker (every task of its fork
 * trees) share (Phase 4's exit test, docs/phase4-plan.md *Testing*): the
 * layout of the tagged pages, and the commands the root posts in each
 * task's work page.
 *
 * Each task has COW_PAGES pages of anonymous memory from mm at COW_VA,
 * each in COW_SLOTS slots, one per writer thread. A slot written holds its
 * tag (the task's record, a generation the root hands out, the writer), the
 * slot's address, words made from those two, and a checksum of the rest. A
 * slot never written is zeroes. The root keeps what each slot must hold and
 * compares what a task reads. */
#ifndef JOIST_USER_COW_H
#define JOIST_USER_COW_H

#include <stdint.h>
#include "work.h"

#define COW_VA     MMC_VA
#define COW_PAGES  32u
#define COW_SLOTS  2u   /* the main thread writes slot 0, the helper slot 1 */
#define COW_WORDS  (512u / COW_SLOTS)
#define COW_HELPER_TOP (0x400000ull - 6u * 0x1000ull) /* the helper's stack, below the main one's */
#define COW_BAD    1u   /* a slot read back that isn't consistent (a tag is never 1) */

/* A tag: the task's record, the generation, the slot. */
static inline uint64_t cow_tag(uint64_t rec, uint64_t gen, uint64_t slot)
{
    return rec << 48 | (gen & 0xffffffffffull) << 2 | 2u | slot;
}

static inline uint64_t cow_mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

/* The seeded choices: the page of a write. */
static inline uint64_t cow_next(uint64_t *s)
{
    *s = cow_mix(*s);
    return *s;
}

static inline volatile uint64_t *cow_slot(uint64_t page, uint64_t slot)
{
    return (volatile uint64_t *)(uintptr_t)(COW_VA + page * 0x1000ull) + slot * COW_WORDS;
}

/* Write a slot with tag: the tag, its address, the words, the checksum. */
static inline void cow_write(uint64_t page, uint64_t slot, uint64_t tag)
{
    volatile uint64_t *p = cow_slot(page, slot);
    uint64_t va = (uint64_t)(uintptr_t)p;
    uint64_t sum = tag + va;
    p[0] = tag;
    p[1] = va;
    for (unsigned i = 2; i < COW_WORDS - 1u; i++) {
        uint64_t v = cow_mix(tag ^ va ^ i);
        p[i] = v;
        sum += v;
    }
    p[COW_WORDS - 1u] = sum;
}

/* What a slot holds: its tag, 0 if it is zeroes, COW_BAD if it is neither. */
static inline uint64_t cow_read(uint64_t page, uint64_t slot)
{
    volatile uint64_t *p = cow_slot(page, slot);
    uint64_t tag = p[0];
    if (tag == 0) {
        for (unsigned i = 1; i < COW_WORDS; i++) {
            if (p[i] != 0) {
                return COW_BAD;
            }
        }
        return 0;
    }
    uint64_t va = (uint64_t)(uintptr_t)p;
    uint64_t sum = tag + va;
    if (p[1] != va) {
        return COW_BAD;
    }
    for (unsigned i = 2; i < COW_WORDS - 1u; i++) {
        uint64_t v = p[i];
        if (v != cow_mix(tag ^ va ^ i)) {
            return COW_BAD;
        }
        sum += v;
    }
    return p[COW_WORDS - 1u] == sum ? tag : COW_BAD;
}

/* The commands (the words of work.h's MMC_W: SEQ, ACK, CMD, A0-A2, RES). */
enum {
    COW_C_WRITE = 1, /* a0 writes by each thread, from seed a1, generation a2; PROG counts them */
    COW_C_VERIFY,    /* every slot read into OBS; RES the number that are COW_BAD */
    COW_C_FORK,      /* a child made (mm's CLONE) and started: RES 0, or how it failed
                      * (COW_E_*); INCLONE is set from just before the CLONE */
    COW_C_EXIT       /* the helper ended, then the task's thread */
};
enum { COW_E_REFUSED = 1, COW_E_FAILED, COW_E_QUOTA }; /* COW_E_FAILED: the step in bits
                                                         * 8-15, the kernel's or mm's result
                                                         * above. COW_E_QUOTA: the task's
                                                         * quota couldn't pay for its child's
                                                         * thread; the clone was cancelled */
#define COW_SEED2(s) ((s) ^ 0x5bd1e9955bd1e995ull) /* the helper's seed */
/* More words of the work page: the main thread's to the helper, and back. */
enum { COW_PROG = 16, COW_INCLONE, COW_HSEQ, COW_HACK, COW_HN, COW_HSEED, COW_HGEN, COW_HEXIT };
#define COW_OBS(w, page, slot) MMC_W(w, 64u + (page) * COW_SLOTS + (slot))

#endif /* JOIST_USER_COW_H */
