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
#define MM_REGIONS 256u /* per task: a glibc program's heap, TLS, each thread's stack and
                          * guard (16 until 2026-11-08: about 5 threads; the owner's choice) */
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
#ifndef MM_KREGIONS
#define MM_KREGIONS 128u /* region capabilities (0030) mm holds, in all: the kernel's
                          * JOIST_MAX_REGIONS */
#endif
#define MM_REGION_EXTENTS 8u /* the kernel's JOIST_REGION_EXTENTS */
#define MM_REGION_TABLES 32u /* the kernel's JOIST_REGION_TABLES */
#ifndef MM_TREE_RESERVE
#define MM_TREE_RESERVE 32u /* a new tree's reservation, in pages (0028) */
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
#define MM_KE_RANGE 1
#define MM_KE_OCCUPIED 3
#define MM_KE_NOMEM 5
#define MM_KE_STATE 13

/* A region capability's right to map in it (JOIST_R_MAP): all a task gets. */
#define MM_R_MAP 2u
/* map_query's answer per page (JOIST_QUERY_*): present, the permissions
 * (MM_W, MM_X), and for the frame named, its page offset in bits 32-63. */
#define MM_QUERY_MAX 64u
#define MM_QUERY_PRESENT 0x8u
#define MM_QUERY_FRAME 0x10u

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
    /* Regions (0030), each as the kernel's call of that name. */
    int (*region_create)(void *ctx, uint32_t task_slot, uint64_t lo, uint32_t tables, unsigned ceil,
                         uint32_t dst);
    int (*region_add)(void *ctx, uint32_t region_slot, uint32_t frame_slot, uint64_t off, uint64_t n,
                      uint32_t *ext);
    int (*region_map)(void *ctx, uint32_t region_slot, uint32_t ext, uint64_t page, uint64_t n,
                      uint64_t va, unsigned perms);
    int (*query)(void *ctx, uint32_t task_slot, uint64_t va, uint64_t n, uint32_t frame_slot,
                 uint64_t *out);
    int (*give)(void *ctx, uint32_t task_slot, uint32_t src, uint32_t dst, unsigned mask);
    int (*kill)(void *ctx, uint32_t slot);
    int (*del)(void *ctx, uint32_t slot);
    /* The kernel's pending destruction run to its end: 1 if it got there. */
    int (*finish)(void *ctx);
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

/* A tree (0028): an adopted task and the tasks forked from it. Every page
 * is charged to the tree of the task that made it, for as long as it is
 * mm's; a tree past its reservation takes pages only from what no tree has
 * reserved, and never past its ceiling. A tree lasts until it has neither
 * tasks nor pages; with no tasks its reservation is given back. */
struct mm_tree {
    uint8_t live;
    uint32_t tasks;
    uint64_t pages;     /* charged to it */
    uint64_t reserve;   /* pages kept for it */
    uint64_t ceil;      /* pages it may have */
    uint32_t ceil_tasks;
};

struct mm_task {
    uint32_t slot; /* mm's task capability (CONTROL); 0 with live == 0 */
    uint8_t live;
    uint32_t tree; /* its tree's index */
    struct mm_region reg[MM_REGIONS];
    struct mm_map map[MM_TASK_PAGES]; /* sorted by va */
    uint32_t nmap;
    uint32_t rpages; /* pages of its region capabilities' extents: with nmap, at
                      * most MM_TASK_PAGES */
};

/* One of mm's pages: which frame holds it, and who maps it. */
struct mm_page {
    uint32_t frame;  /* index into mm.frame */
    uint32_t off;    /* the page's offset in it */
    uint32_t refs;   /* mappings of it; 0 and kreg 0: free */
    uint32_t next;   /* the free list (an index plus one) */
    uint32_t kreg;   /* the region capability whose extent holds it, plus one */
    uint32_t tree;   /* the tree it is charged to, plus one; 0 while free */
};

/* A region capability (0030) mm made for a task: the kernel's object, in
 * mm's slot kreg_slot plus its index (every right), and in the task's slot
 * tslot (MAP alone). Its extents are runs of mm's pages, each in one frame.
 * A region killed whose unmapping wasn't seen to finish is lost: its pages
 * stay out of the pool and its record is never used again. */
struct mm_kextent {
    uint32_t page; /* mm's first page (an index into mm.pg) */
    uint32_t n;
};
struct mm_kregion {
    uint8_t live;
    uint8_t lost;
    uint32_t task;  /* its task's index */
    uint32_t tslot; /* the task's slot holding it: how the task names it */
    uint64_t lo;    /* first page, on a table: MM_SPAN */
    uint32_t tables;
    unsigned perms; /* its ceiling: MM_W, MM_X */
    uint32_t next;  /* extents */
    struct mm_kextent ext[MM_REGION_EXTENTS];
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
    struct mm_tree tree[MM_MAX_TASKS];
    uint64_t lost; /* pages of lost regions: out of the pool for good */
    struct mm_kregion kreg[MM_KREGIONS];
    uint32_t kreg_slot; /* mm's slot for kreg[0]'s capability: the server sets it */
};

void mm_init(struct mm *m, const struct mm_kops *k);
/* Give mm a frame to manage: its pages join the free pool. */
int mm_add_frame(struct mm *m, uint32_t frame_slot, uint64_t npages);
/* Serve a task mm holds a task capability for, in slot: its index, the
 * badge mm gives it being the index plus one. An adopted task starts a tree
 * of its own, with the default limits (MM_TREE_RESERVE; half the pool and
 * half the task records): MM_E_NOMEM if there is no record, or its
 * reservation doesn't fit beside the others'. */
int mm_task_add(struct mm *m, uint32_t slot, uint32_t *t);
/* A clone's child (before mm_clone), in its parent's tree from now on:
 * MM_E_NOMEM past the tree's task ceiling, or no record. */
int mm_task_add_child(struct mm *m, uint32_t slot, uint32_t parent, uint32_t *t);
/* The limits of task t's tree (MM_SET_LIMIT): MM_E_NOMEM if the reservation
 * doesn't fit beside the others'. A ceiling below what it holds refuses
 * only its next pages. */
int mm_set_limit(struct mm *m, uint32_t t, uint64_t reserve, uint64_t ceil, uint32_t ceil_tasks);
/* The task is gone (its badge's no-senders): its pages' references go. */
void mm_task_gone(struct mm *m, uint32_t t);

/* Requests (0023). */
int mm_map_anon(struct mm *m, uint32_t t, uint64_t va, uint64_t n, unsigned perms);
int mm_unmap(struct mm *m, uint32_t t, uint64_t va, uint64_t n);
int mm_protect(struct mm *m, uint32_t t, uint64_t va, uint64_t n, unsigned perms);
/* MM_CLONE's records (0023): child is a task mm has just been given, empty
 * (the server made it and gave it its badge). The parent's writable private
 * pages are made read-only first, then every page is mapped into the child
 * (read-only if private: copy on write; as it is if shared). Its region
 * capabilities are made again for the child, at the same slots, with new
 * pages holding copies and mapped where the parent's are (0030: never
 * COW-shared). */
int mm_clone(struct mm *m, uint32_t parent, uint32_t child);
/* MM_SHARE: the pages of [va, va + n) of task t, which must lie in one of
 * its regions, mapped into task o at [ova, ova + n) too, where o has no
 * region; both become shared regions. A page t shares by COW is copied
 * first, and one not there yet is made, so both map the same page. */
int mm_share(struct mm *m, uint32_t t, uint64_t va, uint64_t n, uint32_t o, uint64_t ova);

/* Region capabilities (0030), each named by the slot of task t's space that
 * holds it. NEW: an empty region of `tables` tables from page lo (on a
 * table), allowing perms (MM_W, MM_X), given to t in tslot with MAP alone.
 * GROW: n zeroed pages as its next extent, its index in *ext. FREE: killed,
 * and its pages back once the kernel has unmapped them. */
int mm_region_new(struct mm *m, uint32_t t, uint64_t lo, uint32_t tables, unsigned perms,
                  uint32_t tslot);
int mm_region_grow(struct mm *m, uint32_t t, uint32_t tslot, uint64_t n, uint32_t *ext);
int mm_region_free(struct mm *m, uint32_t t, uint32_t tslot);

/* A fault in task t at page va with the kernel's access bits: 1 to resume
 * the thread (the page is there now), 0 to end it. */
int mm_fault(struct mm *m, uint32_t t, uint64_t va, unsigned access);

/* mm's records, checked (for its tests and mm's own self-check): 0 if
 * sound, else the number of faults found, the first in msg. */
unsigned mm_check(const struct mm *m, char *msg, unsigned msglen);

#endif
