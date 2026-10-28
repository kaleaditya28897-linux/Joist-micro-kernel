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
 * or, for a fault, JOIST_FAULT_RESUME (0) or not. */
#ifndef JOIST_MM_PROTO_H
#define JOIST_MM_PROTO_H

#define MM_SLOT 8u /* in every task mm serves: its capability to mm */

#define MM_REQ_BASE 0x10000u
enum {
    MM_REQ_MAP_ANON = MM_REQ_BASE + 1u, /* va page, pages, permissions */
    MM_REQ_UNMAP,                       /* va page, pages */
    MM_REQ_PROTECT,                     /* va page, pages, permissions */
    MM_REQ_CLONE,                       /* -> a task capability to the child (D2) */
    MM_REQ_SHARE                        /* (D2) */
};
/* The root's (MM_ROOT_BADGE only). */
enum {
    MM_REQ_INIT = MM_REQ_BASE + 0x100u, /* pool pages, table pages: what it was given */
    MM_REQ_ADOPT,                       /* the slot of mm's space the root put a task
                                         * capability (CONTROL) in -> r[1] its badge */
    MM_REQ_STATUS                       /* -> r[1] tasks served, r[2] free pages,
                                         * r[3] faults mm_check finds in its records */
};

#define MM_ROOT_BADGE 0x7fff0000u /* above any task's (a task's is its index plus one) */

/* The slots of mm's own space the root fills before starting it. */
enum {
    MM_S_EP = 0,     /* its endpoint: RECV, SEND, GRANT */
    MM_S_SELF = 1,   /* a task capability (CONTROL) to mm itself: to map its pool */
    MM_S_POOL = 2,   /* the frame whose pages it manages */
    MM_S_TABLES = 3, /* a frame of pages to lend its tasks for their page tables */
    MM_S_TMP = 4,    /* mm's own: a badged capability on its way to a task */
    MM_S_FREE = 16,  /* mm's own from here (the table runs it lends) ... */
    MM_S_ROOT = 64   /* ... to here; from here on, the root's (task capabilities) */
};

#define MM_POOL_VA 0x10000000ull  /* where mm maps its pool in its own space (its first
                                   * gigabyte, where the loader lent it tables) */
#define MM_TABLE_PAGES 16u        /* pages lent to each task for its page tables */

#endif
