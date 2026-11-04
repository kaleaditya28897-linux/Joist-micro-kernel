/* SPDX-License-Identifier: GPL-3.0-only */
/* mm_proto.h - how tasks and the root talk to the memory manager
 * (decision 0023). Shared by the server (servers/mm/mm.c), the root task
 * that starts it, and its clients.
 *
 * mm has one endpoint. Every task it serves holds one capability to it,
 * with SEND and a badge naming the task, in MM_SLOT; it is also the task's
 * fault handler (JOIST_FAULT_NO_THREAD_CAP), so a fault arrives as a
 * message from that badge. The root holds one with MM_ROOT_BADGE, for the
 * requests only it may make.
 *
 * A request: r[0] the request (MM_REQ_*), r[1]-r[3] its arguments (pages,
 * not bytes). A fault message's r[0] is the fault kind and access
 * (JOIST_FAULT_KIND, JOIST_FAULT_ACCESS: below 0x10000), so the two can't
 * be confused. The reply: r[0] the result (MM_OK or an MM_E_*: mmlib.h)
 * or, for a fault, JOIST_FAULT_RESUME (0) or not.
 *
 * A task's id (MM_REQ_ID) is its index in mm's records and a generation
 * that mm bumps each time the index is used again: an id names one task,
 * never a later one with its index. It carries no authority; SHARE takes
 * it only with the target's consent (MM_REQ_SHARE_ACCEPT: 0023's
 * amendment of 2026-10-29). */
#ifndef JOIST_MM_PROTO_H
#define JOIST_MM_PROTO_H

#define MM_SLOT 8u /* in every task mm serves: its capability to mm */

#define MM_REQ_BASE 0x10000u
enum {
    MM_REQ_MAP_ANON = MM_REQ_BASE + 1u, /* va page, pages, permissions */
    MM_REQ_UNMAP,                       /* va page, pages */
    MM_REQ_PROTECT,                     /* va page, pages, permissions */
    MM_REQ_CLONE,                       /* -> r[1] the child's id, and a task capability to
                                         * it (CONTROL, GRANT) in the reply's caps; the
                                         * child is pending until MM_REQ_CLONE_DONE */
    MM_REQ_SHARE,                       /* va page, pages, the target's id: only if the
                                         * target accepted exactly that */
    MM_REQ_ID,                          /* -> r[1] the caller's own id */
    MM_REQ_SHARE_ACCEPT,                /* the sharer's id, va page, pages: where this
                                         * task takes its pages (one pending per task, a
                                         * new one replacing it; 0 pages withdraws it) */
    MM_REQ_CLONE_DONE,                  /* the child's id, MM_CLONE_KEEP or _CANCEL: the
                                         * parent's fork finished (its child's thread runs)
                                         * or given up (mm destroys the child) */
    /* Regions (0030), each named by the slot of the caller's space that
     * holds it. A fork's child has its parent's, copied, at the same slots. */
    MM_REQ_REGION_NEW,                  /* first page (on a 512-page table), tables |
                                         * permissions << 8, the slot: an empty region,
                                         * its capability (MAP alone) put in that slot */
    MM_REQ_REGION_GROW,                 /* the slot, pages -> r[1] the new extent's index:
                                         * zeroed pages, mapped by the task itself */
    MM_REQ_REGION_FREE                  /* the slot: the region killed, its pages back once
                                         * unmapped; the dead capability stays for the
                                         * task to delete */
};
/* A child still pending when its parent goes is destroyed with it: a fork
 * not confirmed never happened (0023's amendment of 2026-10-29, "orphans"). */
enum { MM_CLONE_KEEP = 0, MM_CLONE_CANCEL = 1 };
#define MM_ID(t, gen) ((uint64_t)(gen) << 16 | (t))
#define MM_ID_TASK(id) ((uint32_t)((id) & 0xffffu))
#define MM_ID_GEN(id)  ((id) >> 16)
/* The root's (MM_ROOT_BADGE only). */
enum {
    MM_REQ_INIT = MM_REQ_BASE + 0x100u, /* pool pages, table pages: what it was given.
                                         * MM_E_NOMEM unless MM_S_TABLES holds a run of
                                         * MM_TABLE_PAGES for every task record, plus one
                                         * page (0028) */
    MM_REQ_ADOPT,                       /* the slot of mm's space the root put a task
                                         * capability (CONTROL) in -> r[1] its badge. It
                                         * starts a tree (0028); MM_E_NOMEM if the tree's
                                         * reservation doesn't fit beside the others' */
    MM_REQ_STATUS,                      /* -> r[1] tasks served, r[2] free pages,
                                         * r[3] faults mm_check finds in its records */
    MM_REQ_SET_LIMIT                    /* a task's badge; its tree's reservation |
                                         * page ceiling << 32; its task ceiling (0028;
                                         * proc's in Phase 8) */
};

#define MM_ROOT_BADGE 0x7fff0000u /* above any task's (a task's is its index plus one) */

/* The slots of mm's own space. The root fills the first four before
 * starting it, and slots from MM_S_ROOT to MM_S_CHILD as it hands mm
 * tasks; the rest are mm's. mm adds the second leaf (MM_S_CHILD on) and the
 * third (MM_S_REGION on) at MM_REQ_INIT: a task it makes itself (MM_REQ_CLONE) is in MM_S_CHILD plus
 * its index. Collection deletes a task's slot, whichever range it is in. */
enum {
    MM_S_EP = 0,      /* its endpoint: RECV, SEND, GRANT */
    MM_S_SELF = 1,    /* a task capability (CONTROL) to mm itself: to map its pool */
    MM_S_POOL = 2,    /* the frame whose pages it manages */
    MM_S_TABLES = 3,  /* a frame of pages to lend its tasks for their page tables */
    MM_S_TMP = 4,     /* mm's own: a badged capability on its way to a task */
    MM_S_FREE = 16,   /* mm's own from here (the table runs it lends) ... */
    MM_S_ROOT = 64,   /* ... to here; from here, the root's (task capabilities) ... */
    MM_S_CHILD = 128, /* ... to here; from here, the tasks mm made (MM_MAX_TASKS) */
    MM_S_REGION = 256 /* the third leaf: mm's region capabilities (MM_KREGIONS), each with
                       * every right */
};

#define MM_POOL_VA 0x10000000ull  /* where mm maps its pool in its own space (its first
                                   * gigabyte, where the loader lent it tables) */
#ifndef MM_TABLE_PAGES
#define MM_TABLE_PAGES 16u        /* pages lent to each task for its page tables */
#endif
#ifndef MM_CHILD_QUOTA
#define MM_CHILD_QUOTA 32u        /* quota a task mm makes starts with, from mm's own: mm
                                   * pays it and the task object (task_create's donation
                                   * plus one), and gets both back when the task goes. A
                                   * task pays one for each child's thread while that child
                                   * lives, so this bounds its live children too (8 was hit
                                   * by the exit run: 2026-10-30) */
#endif

#endif
