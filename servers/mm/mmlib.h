/* SPDX-License-Identifier: GPL-3.0-only */
/* mmlib.h - the memory manager's logic (decision 0023; Phase 4 step 6).
 *
 * mm keeps, for every task it serves, the regions of its address space it
 * has been asked for, and, for every page it manages, who maps it and how
 * often. It answers faults and requests by calling the kernel through
 * struct mm_kops, so the same code runs in the server (servers/mm/mm.c,
 * through system calls) and on the host against the kernel model (its
 * tests: model/test/test_mm.c).
 *
 * Every table is a fixed size, set at build time (as the kernel's): a
 * request that would overflow one is refused with MM_E_NOMEM. Nothing here
 * allocates.
 *
 * Addresses are page numbers (the kernel's virtual pages), as in the
 * kernel's calls. */
#ifndef JOIST_MM_LIB_H
#define JOIST_MM_LIB_H

#include <stdint.h>

#ifndef MM_MAX_TASKS
#define MM_MAX_TASKS 64u
#endif
#ifndef MM_REGIONS
#define MM_REGIONS 16u /* per task */
#endif
#ifndef MM_TASK_PAGES
#define MM_TASK_PAGES 1024u /* pages a task may have mapped at once */
#endif
#ifndef MM_PAGES
#define MM_PAGES 4096u /* pages mm manages, in all */
#endif
#ifndef MM_FRAMES
#define MM_FRAMES 16u /* frames mm is given */
#endif
#define MM_SPAN 512u   /* the kernel's JOIST_MAP_SPAN: one call's pages */
#define MM_AROUND 16u  /* pages a read fault maps at most (0023) */

/* Results (as joist_err: 0 is success). */
enum { MM_OK = 0, MM_E_RANGE = 1, MM_E_NOMEM = 2, MM_E_STATE = 3, MM_E_KERNEL = 4 };

/* Permissions, as the kernel's (JOIST_MAP_W, JOIST_MAP_X); a region with
 * neither may be read. MM_NONE, alone: no access at all (PROT_NONE, and a
 * guard page: 0024). The kernel maps nothing there and a fault ends the
 * thread, but mm keeps the pages a region had: made accessible again, each
 * is mapped back at its first fault, holding what it held (POSIX's
 * mprotect). */
#define MM_W 1u
#define MM_X 2u
#define MM_NONE 4u

/* The kernel's errors mm tells apart (JOIST_E_*, J_E_*): a page mapped
 * already (map), or not the old page (map_replace). */
#define MM_KE_OCCUPIED 3
#define MM_KE_STATE 13

/* A fault's access, as the kernel's message gives it (JOIST_ACCESS_*). */
#define MM_ACCESS_READ 0x1u
#define MM_ACCESS_WRITE 0x2u
#define MM_ACCESS_EXEC 0x4u
#define MM_ACCESS_PRESENT 0x8u

/* What mm needs of the kernel, all in its own space (slots are mm's). Each
 * returns 0 on success, or the kernel's error (map and map_replace: mm
 * tells MM_KE_OCCUPIED and MM_KE_STATE apart). page() gives mm's own view of one of its pages (in
 * the server, its mapping of the frame; on the host, simulated memory). */
struct mm_kops {
    void *ctx;
    int (*map)(void *ctx, uint32_t frame_slot, uint64_t off, uint64_t n, uint32_t task_slot,
               uint64_t va, unsigned perms);
    int (*map_replace)(void *ctx, uint32_t frame_slot, uint64_t off, uint32_t task_slot,
                       uint64_t va, unsigned perms, uint32_t old_frame_slot, uint64_t old_off);
    int (*unmap)(void *ctx, uint32_t task_slot, uint64_t va, uint64_t n);
    int (*protect)(void *ctx, uint32_t task_slot, uint64_t va, uint64_t n, unsigned perms);
    uint8_t *(*page)(void *ctx, uint32_t frame_slot, uint64_t off);
};

struct mm_region {
    uint64_t va, n;    /* first page, pages; n == 0: unused */
    unsigned perms;    /* MM_W, MM_X, or MM_NONE alone */
    uint8_t shared;    /* shared with another task (mm_share): never COW */
};

struct mm_map {
    uint64_t va;   /* the task's page */
    uint32_t page; /* mm's page (an index into mm.pg) */
};

struct mm_task {
    uint32_t slot; /* mm's task capability (CONTROL); 0 with live == 0 */
    uint8_t live;
    struct mm_region reg[MM_REGIONS];
    struct mm_map map[MM_TASK_PAGES]; /* sorted by va */
    uint32_t nmap;
};

/* One of mm's pages: which frame holds it, and who maps it. */
struct mm_page {
    uint32_t frame;  /* index into mm.frame */
    uint32_t off;    /* the page's offset in it */
    uint32_t refs;   /* mappings of it; 0: free */
    uint32_t next;   /* the free list (an index plus one) */
};

struct mm_frame {
    uint32_t slot;   /* mm's frame capability */
    uint64_t npages;
};

struct mm {
    struct mm_kops k;
    struct mm_frame frame[MM_FRAMES];
    uint32_t nframes;
    struct mm_page pg[MM_PAGES];
    uint32_t npages;
    uint32_t free;   /* the first free page, plus one; 0: none */
    uint32_t nfree;
    struct mm_task task[MM_MAX_TASKS];
};

void mm_init(struct mm *m, const struct mm_kops *k);
/* Give mm a frame to manage: its pages join the free pool. */
int mm_add_frame(struct mm *m, uint32_t frame_slot, uint64_t npages);
/* Serve a task mm holds a task capability for, in slot: its index, the
 * badge mm gives it being the index plus one. */
int mm_task_add(struct mm *m, uint32_t slot, uint32_t *t);
/* The task is gone (its badge's no-senders): its pages' references go. */
void mm_task_gone(struct mm *m, uint32_t t);

/* Requests (0023). */
int mm_map_anon(struct mm *m, uint32_t t, uint64_t va, uint64_t n, unsigned perms);
int mm_unmap(struct mm *m, uint32_t t, uint64_t va, uint64_t n);
int mm_protect(struct mm *m, uint32_t t, uint64_t va, uint64_t n, unsigned perms);
/* MM_CLONE's records (0023): child is a task mm has just been given, empty
 * (the server made it and gave it its badge). The parent's writable private
 * pages are made read-only first, then every page is mapped into the child
 * (read-only if private: copy on write; as it is if shared). */
int mm_clone(struct mm *m, uint32_t parent, uint32_t child);
/* MM_SHARE: the pages of [va, va + n) of task t, which must lie in one of
 * its regions, mapped into task o at [ova, ova + n) too, where o has no
 * region; both become shared regions. A page t shares by COW is copied
 * first, and one not there yet is made, so both map the same page. */
int mm_share(struct mm *m, uint32_t t, uint64_t va, uint64_t n, uint32_t o, uint64_t ova);

/* A fault in task t at page va with the kernel's access bits: 1 to resume
 * the thread (the page is there now), 0 to end it. */
int mm_fault(struct mm *m, uint32_t t, uint64_t va, unsigned access);

/* mm's records, checked (for its tests and mm's own self-check): 0 if
 * sound, else the number of faults found, the first in msg. */
unsigned mm_check(const struct mm *m, char *msg, unsigned msglen);

#endif
