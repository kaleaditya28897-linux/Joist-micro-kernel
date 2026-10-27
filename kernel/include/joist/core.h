/* core.h - the kernel's object layer: public API.
 *
 * Capabilities, endpoints, IPC, notifications, lifetime events, tasks and
 * threads (plan §5.2, decisions 0001, 0003, 0004). Written as the Phase 1
 * host model (model/) and moved into the kernel unchanged in Phase 3
 * (docs/phase3-plan.md, step 2): kernel/core/ is the one source, compiled
 * freestanding into the kernel and on the host for model/'s tests, fuzzer,
 * CBMC proofs and mutations. docs/abi.md documents its semantics.
 *
 * Rules (model/README.md): no malloc, no recursion, no VLAs, fixed-width
 * types, bounded work per operation. All state lives in one caller-provided
 * struct joist_kernel with compile-time capacities, placed by the kernel at
 * boot (or by a test). Invariant numbers (I1...) refer to
 * docs/phase1-plan.md. */
#ifndef JOIST_CORE_H
#define JOIST_CORE_H

#include <stdint.h>

/* Capacities. Compile-time, as the kernel's will be (0003: preallocated, no
 * growth path at runtime). */
#ifndef JOIST_MAX_OBJECTS
#define JOIST_MAX_OBJECTS 256u
#endif
#ifndef JOIST_MAX_CSPACES
#define JOIST_MAX_CSPACES 16u
#endif
/* The capability table is two-level (0001 amendment, item 4): a root of
 * JOIST_CNODE_ROOT entries, each naming a leaf of JOIST_CNODE_LEAF slots.
 * Slot s lives in leaf s / JOIST_CNODE_LEAF. Leaf 0 is part of the space
 * itself (its slot[] array), so every space has it; the others come from a
 * fixed pool of JOIST_MAX_LEAVES, added with joist_cspace_grow and charged
 * to the space's quota like an object. A slot in a leaf that isn't there is
 * out of range (JOIST_E_RANGE), exactly like one past the end: lookups never
 * allocate. Lookup is O(1). */
#ifndef JOIST_CNODE_LEAF
#define JOIST_CNODE_LEAF 128u
#endif
#ifndef JOIST_CNODE_ROOT
#define JOIST_CNODE_ROOT 64u
#endif
#ifndef JOIST_MAX_LEAVES
#define JOIST_MAX_LEAVES 64u
#endif
#define JOIST_CSPACE_SLOTS (JOIST_CNODE_ROOT * JOIST_CNODE_LEAF)
/* Priorities (§5.6: 256). A smaller number only for the bounded proofs
 * (CBMC), which can't unwind loops over 256. */
#ifndef JOIST_PRIOS
#define JOIST_PRIOS 256u
#endif
#define JOIST_PRIO_WORDS ((JOIST_PRIOS + 63u) / 64u)

/* CPUs the scheduler places threads on (Phase 3 step 9). */
#ifndef JOIST_MAX_CPUS
#define JOIST_MAX_CPUS 8u
#endif
#ifndef JOIST_MAX_THREADS
#define JOIST_MAX_THREADS 32u
#endif
/* Message registers: 0001's open item 1 (number and width). */
#ifndef JOIST_MSG_REGS
#define JOIST_MSG_REGS 4u
#endif
/* Per-badge send-capability accounting entries (no-senders, I4), allocated
 * at mint. */
#ifndef JOIST_MAX_BADGE_ACCTS
#define JOIST_MAX_BADGE_ACCTS 64u
#endif
/* Dead-name notification registrations (I5). */
/* The page tables' shape (*page tables*, below): JOIST_PT_BITS index bits
 * per level (the hardware's 9, 512 entries a node), JOIST_PT_LEVELS levels
 * of nodes under a top level of JOIST_PT_TOP entries. Only CBMC builds them
 * smaller (model/cbmc/run.sh), to keep its state small; the kernel's
 * page-table code (kernel/mem/vm.c) requires the hardware's. */
#ifndef JOIST_PT_BITS
#define JOIST_PT_BITS 9u
#endif
#define JOIST_PT_LEVELS 3u
#ifndef JOIST_PT_TOP
#define JOIST_PT_TOP 256u /* top-level entries covering the user half */
#endif
/* Pages one entry covers at `level` (0 the last; JOIST_PT_LEVELS the top). */
#define JOIST_PT_SHIFT(level) (JOIST_PT_BITS * (level))
/* Pages in the user half of an address space (x86_64: 2^47 bytes of 4 KiB):
 * JOIST_PT_TOP top-level entries of 2^27 pages each. */
#define JOIST_USER_PAGES ((uint64_t)JOIST_PT_TOP << JOIST_PT_SHIFT(JOIST_PT_LEVELS))
#define JOIST_PAGE_SIZE 4096u /* bytes in a page: addresses that the core sees in bytes */
#ifndef JOIST_MAX_DEADNAMES
#define JOIST_MAX_DEADNAMES 32u
#endif

/* Timeout meaning "no deadline". 0 means "do not block". */
#define JOIST_FOREVER UINT64_MAX

typedef enum {
    JOIST_OK = 0,
    JOIST_E_RANGE,    /* slot or capability-space index out of range */
    JOIST_E_EMPTY,    /* source slot holds no capability */
    JOIST_E_OCCUPIED, /* destination slot already holds a capability */
    JOIST_E_RIGHTS,   /* the operation needs a right the capability lacks */
    JOIST_E_NOMEM,    /* quota or object table exhausted (I3: never blocks) */
    JOIST_E_BADGE,    /* badge already set, or badge value invalid */
    JOIST_E_TYPE,     /* wrong object type for the operation */
    JOIST_E_DEAD,     /* the capability space has been destroyed */
    JOIST_E_TIMEOUT,  /* a blocking operation's deadline passed */
    JOIST_E_GONE,     /* the endpoint was destroyed while the thread waited (I10) */
    JOIST_E_PEER,     /* the peer of a call is gone: server died holding the reply
                         capability, or the caller stopped waiting (I6, I10) */
    JOIST_E_NOREPLY,  /* Reply without a reply capability (I6: single use) */
    JOIST_E_STATE,    /* thread is not running, or holds an unused reply capability */
    JOIST_E_ABORTED,  /* the blocking operation was cancelled by thread_abort or
                         thread_suspend (0010): glibc's EINTR */
    JOIST_BLOCKED     /* not an error: the thread is now blocked; the outcome is
                         left in its record when it wakes */
} joist_err;

typedef enum {
    JOIST_OBJ_NONE = 0,
    JOIST_OBJ_ENDPOINT,
    JOIST_OBJ_NOTIFICATION,
    JOIST_OBJ_TASK,   /* a capability space with its threads (0004) */
    JOIST_OBJ_THREAD, /* a thread; its capability is 0010's thread control capability */
    JOIST_OBJ_REPLY,  /* holds one saved reply capability: a deferred reply (0001) */
    JOIST_OBJ_SCHED,  /* a scheduling context: priority, budget, period (§5.6) */
    JOIST_OBJ_FRAME   /* a range of physical pages (0013: memory as capabilities) */
} joist_obj_type;

/* Priorities (§5.6): 0 lowest, JOIST_PRIO_MAX highest. */
#define JOIST_PRIO_MAX 255u

/* Rights bits (0001). */
typedef uint8_t joist_rights;
#define JOIST_R_SEND      ((joist_rights)0x1u)
#define JOIST_R_RECV      ((joist_rights)0x2u)
#define JOIST_R_SEND_ONCE ((joist_rights)0x4u)
#define JOIST_R_GRANT     ((joist_rights)0x8u)
#define JOIST_R_ALL       ((joist_rights)0xfu)
/* On task and thread capabilities, the SEND bit means "control": create
 * threads in the task, destroy it, kill the thread (0004, 0010). */
#define JOIST_R_CONTROL JOIST_R_SEND
/* On frames (and regions, when they exist: docs/phase4-plan.md, choice 7),
 * the SEND_ONCE bit is the kill right: joist_obj_kill (decision 0022).
 * Boot frames have it; a derive or a transfer can mask it away like any
 * right, so copies can be handed out that can't revoke. */
#define JOIST_R_KILL JOIST_R_SEND_ONCE

/* Badge. 0 means "unbadged". The width is 0001's open item 2; 64 bits until
 * the model settles it. */
typedef uint64_t joist_badge;

typedef uint32_t joist_slot;   /* index into a capability space */
typedef uint32_t joist_cspace; /* index of a capability space */

/* A capability. obj is an index into the object table plus one (0 = empty
 * slot), so an all-zero slot is empty. */
struct joist_cap {
    uint32_t obj;
    uint8_t type; /* joist_obj_type, cached from the object */
    joist_rights rights;
    joist_badge badge;
};

struct joist_object {
    uint8_t type;   /* joist_obj_type; JOIST_OBJ_NONE = free */
    uint32_t refs;  /* capabilities naming this object, across all spaces (I1) */
    uint32_t donor; /* capability space charged for this object (0004) */
    /* Tasks and threads: the space or thread record this object names (index
     * plus one). Such an object is "dead" (the dead field below) once its task
     * or thread is gone; capabilities to it then fail with JOIST_E_GONE. */
    uint32_t target;
    /* Reply objects: while bound, target is the waiting caller plus one and
     * rseq the caller's call_seq at delivery (as a thread's reply_seq). */
    uint32_t rseq;
    /* One FIFO queue of blocked threads: an endpoint's senders or receivers,
     * or a notification's waiters (0001). Thread index plus one; 0 = empty. */
    uint8_t qkind; /* joist_queue_kind */
    uint32_t qhead;
    uint32_t qtail;
    /* Endpoints. An endpoint *dies* when its last capability with RECV is
     * deleted (the server is gone), while send capabilities may remain: their
     * use then fails with JOIST_E_GONE (I5). The object is freed when refs
     * reaches 0. A frame dies when it is killed (joist_obj_kill, 0022). */
    uint32_t recv_caps; /* capabilities with RECV naming it */
    uint8_t dead;
    uint32_t ns_notif;   /* no-senders notification bound to it: object index + 1 */
    joist_badge ns_bits; /* the bit(s) that binding signals: its capability's badge */
    /* Fired no-senders accounts not yet collected by joist_nosenders_next, a
     * FIFO linked through the accounts themselves (index + 1), so it needs
     * no memory of its own (I3, I4). */
    uint32_t ns_first;
    uint32_t ns_last;
    /* Notifications: the pending bit-set. Width: 0001's open item 3. */
    uint64_t word;
    /* Scheduling contexts (target: the owning thread, plus one, 0 if
     * unbound). Budget and period in time units (the kernel's: nanoseconds).
     * `remaining` is what is left of the budget in the current period, and
     * `refill_at` when the next period starts and the budget is whole again
     * (0: no period has started yet). In any period at most `budget` runs on
     * this context, whoever holds it. */
    uint8_t prio;
    uint64_t budget;
    uint64_t period;
    uint64_t remaining;
    uint64_t refill_at;
    /* Frames: the first physical page and the number of pages. */
    uint64_t base;
    uint64_t npages;
    uint64_t mapcount;  /* present page entries naming it (0020) */
    uint32_t lent;      /* the space it is lent to, plus one; 0: not lent (0021) */
    uint32_t lent_next; /* the next frame lent to that space, object plus one */
    uint64_t water;     /* lent: its pages taken for tables so far */
    /* Its last capability is gone, but it is still mapped or lent: no new
     * mapping, and freed when neither (0020). refs is 0 meanwhile. */
    uint8_t dying;
};

/* On a frame capability the RECV bit means "may be mapped writable"; SEND is
 * CONTROL (map, split), as on tasks and threads. */
#define JOIST_R_WRITE JOIST_R_RECV

/* Mapping permissions. Readable is implied; never both (W^X). */
#define JOIST_MAP_W 1u
#define JOIST_MAP_X 2u

/* ---- page tables (0020, 0021; Phase 4 step 2) ----
 * The record of what is mapped is a tree per space that mirrors the
 * hardware's: a top level of JOIST_PT_TOP entries in the space's state, then
 * JOIST_PT_LEVELS levels of nodes, each one page of a frame lent to the space
 * (0021) and paired with the hardware table it mirrors (two pages per table).
 * Level 2 is under the top, level 0 the last. An interior entry names both
 * pages of its child: JOIST_PT_NODE(shadow page, hardware page), 0 = none. A
 * last-level entry is 0020's record: the frame, the page in it, the
 * permissions and the rights ceiling. */
#define JOIST_PT_ENTRIES (1u << JOIST_PT_BITS) /* entries per node, every level */
#define JOIST_PT_NODE(shadow, hw) (((uint64_t)(shadow) + 1u) | (((uint64_t)(hw) + 1u) << 32))
#define JOIST_PT_SHADOW(e) (((e) & 0xffffffffu) - 1u)
#define JOIST_PT_HW(e) (((e) >> 32) - 1u)
/* Page numbers a lent frame may hold: they must fit an interior entry. */
#define JOIST_PT_PFN_LIMIT 0xffffffffull
/* A last-level entry. */
#define JOIST_PTE_W 0x1u      /* = JOIST_MAP_W */
#define JOIST_PTE_X 0x2u      /* = JOIST_MAP_X */
#define JOIST_PTE_CEIL_W 0x4u /* the frame capability used allowed writing */
#define JOIST_PTE_P 0x8u      /* present */
#define JOIST_PTE(frame, off, perms, ceil_w)                                                       \
    ((uint64_t)JOIST_PTE_P | (uint64_t)(perms) | ((ceil_w) ? (uint64_t)JOIST_PTE_CEIL_W : 0u) |    \
     ((uint64_t)(frame) << 16) | ((uint64_t)(off) << 32))
#define JOIST_PTE_FRAME(e) ((uint32_t)(((e) >> 16) & 0xffffu))
#define JOIST_PTE_OFF(e) ((e) >> 32)
#define JOIST_PTE_PERMS(e) ((unsigned)((e) & (JOIST_PTE_W | JOIST_PTE_X)))
/* Pages a frame may have: the page offset must fit a last-level entry. */
#define JOIST_FRAME_PAGES_MAX 0xffffffffull

typedef enum {
    JOIST_Q_NONE = 0,
    JOIST_Q_SENDERS,
    JOIST_Q_RECEIVERS,
    JOIST_Q_WAITERS /* threads waiting on a notification */
} joist_queue_kind;

/* No-senders accounting (I4, §5.4): send-capable capabilities per
 * (endpoint, badge), allocated when mint creates the first one. When the
 * count reaches 0 the account is freed, or, if a notification is bound to
 * the endpoint, it is marked fired and queued on the endpoint until the owner
 * collects the badge (joist_nosenders_next). Deriving and deleting never
 * allocate. */
struct joist_badge_acct {
    uint32_t obj; /* endpoint object index + 1; 0 = free entry */
    joist_badge badge;
    uint32_t send_caps;
    uint8_t fired; /* the last sender is gone; waiting to be collected */
    uint32_t next; /* while fired: the next fired account of the endpoint, plus one */
};

/* A dead-name notification registration (I5): when endpoint ep dies, signal
 * notification notif with bits, once, and free the entry. */
struct joist_deadname {
    uint32_t ep;    /* object index + 1; 0 = free entry */
    uint32_t notif; /* object index + 1 */
    joist_badge bits;
};

typedef enum {
    JOIST_T_FREE = 0,
    JOIST_T_RUNNING,
    JOIST_T_BLOCKED_SEND,  /* queued on an endpoint with a Call not yet received */
    JOIST_T_BLOCKED_RECV,  /* queued on an endpoint to receive */
    JOIST_T_BLOCKED_REPLY, /* Call delivered; waiting for the server's Reply */
    JOIST_T_BLOCKED_WAIT,  /* queued on a notification */
    JOIST_T_SUSPENDED      /* stopped by thread_suspend, or by a fault nobody resolved */
} joist_thread_state;

/* Capabilities one message can carry (Phase 3 step 3; docs/abi.md). */
#ifndef JOIST_MSG_CAPS
#define JOIST_MSG_CAPS 2u
#endif
/* "No capability arrived in this position" in a received message. */
#define JOIST_SLOT_NONE UINT32_MAX
/* In place of a task slot (joist_map, joist_unmap): the caller's own space.
 * The root task has no task object (0013), and every task may map memory it
 * holds into itself. */
#define JOIST_SLOT_SELF (UINT32_MAX - 1u)

/* A thread's register state (0010): one fixed record per architecture, never
 * a family of flavours. On x86_64: pc (RIP), sp (RSP), flags (RFLAGS, of
 * which the kernel lets user code set only the arithmetic and direction
 * flags), tls (the FS base), and r[] = RAX, RBX, RCX, RDX, RSI, RDI, RBP,
 * R8-R15 (r[15] unused). The kernel saves a thread's user state here on
 * every entry and restores it from here on every exit, so reading and
 * writing it through a thread control capability is reading and writing
 * the thread. */
#ifndef JOIST_REGS
#define JOIST_REGS 16u
#endif
struct joist_regs {
    uint64_t pc;
    uint64_t sp;
    uint64_t flags;
    uint64_t tls; /* the TLS base is a thread attribute, not a GDT call (0010) */
    uint64_t r[JOIST_REGS];
};

/* Hooks through which the core tells the architecture what to carry out
 * (all optional: the host model leaves them NULL). The core decides and the
 * kernel only executes, so the hooks can't fail: everything they need is
 * sized from the core's own limits at boot. */
struct joist_hooks {
    void *ctx;
    /* A space came alive (task_create, boot) or is gone (destruction
     * finished): build or free its page tables. */
    void (*space_new)(void *ctx, uint32_t space);
    void (*space_gone)(void *ctx, uint32_t space);
    /* npages pages from physical page `pa` appear at virtual page va of the
     * space, or disappear from it. */
    /* Page tables (0020): a table at `level` (2 under the top, 0 the last)
     * now covers va, in physical page pfn, zeroed; or it is gone. */
    void (*table_new)(void *ctx, uint32_t space, uint64_t va, unsigned level, uint64_t pfn);
    void (*table_gone)(void *ctx, uint32_t space, uint64_t va, unsigned level);
    /* npages pages from physical page `pa` appear at virtual page va of the
     * space (replacing what was there if `replace`: the TLBs then need
     * flushing), or disappear from it, or change permissions. Called for runs
     * that are wholly present. */
    void (*map)(void *ctx, uint32_t space, uint64_t va, uint64_t pa, uint64_t npages,
                unsigned perms, int replace);
    void (*unmap)(void *ctx, uint32_t space, uint64_t va, uint64_t npages);
    void (*protect)(void *ctx, uint32_t space, uint64_t va, uint64_t npages, unsigned perms);
    /* Where the core can reach physical page pfn, to keep its page-table
     * nodes in pages lent to a space (0021): the kernel's direct map, or a
     * simulated memory on the host. NULL: no page can be lent. */
    void *(*page)(void *ctx, uint64_t pfn);
    /* Thread t, on CPU `cpu`, is about to be stopped, destroyed, or have
     * its registers read or written from elsewhere: get it off that CPU
     * now, its registers in its record (step 9). Not called for a thread on
     * no CPU; the kernel ignores it for its own CPU's thread, whose
     * registers it has already. */
    void (*off_cpu)(void *ctx, uint32_t t, uint32_t cpu);
    /* Copy `words` 64-bit words of a message from the buffer at from_va in
     * one space to the buffer at to_va in another: 1 if done, 0 if it
     * can't be (a page not mapped as it must be), then nothing written.
     * Bounded by JOIST_MSG_WORDS. NULL in the host model: copied. */
    int (*copy_msg)(void *ctx, uint32_t from_space, uint64_t from_va, uint32_t to_space,
                    uint64_t to_va, uint32_t words);
};
/* Pages one call may cover: one page table (x86_64: 512 pages, 2 MiB). A
 * run lies within one such aligned region, so it needs at most one new table
 * at each level and one TLB shootdown. Larger regions take several calls. */
#define JOIST_MAP_SPAN JOIST_PT_ENTRIES
/* The frame sweep's cursor: last-level tables per space. */
#define JOIST_PT_SWEEP_SPACE (JOIST_USER_PAGES / JOIST_PT_ENTRIES)

/* Fault messages (0009): what a fault handler receives, in the message
 * registers: r[0] the kind, r[1] the address (page faults), r[2] the
 * architecture's error code; capability 0, a thread control capability for
 * the faulting thread. The handler's reply says what happens: r[0] ==
 * JOIST_FAULT_RESUME resumes it, anything else terminates it. */
#define JOIST_FAULT_PAGE   1u
#define JOIST_FAULT_GP     2u
#define JOIST_FAULT_UD     3u
#define JOIST_FAULT_MATH   4u
#define JOIST_FAULT_OTHER  5u
#define JOIST_FAULT_RESUME 0u

/* The IPC buffer (0001 amendment, item 1; Phase 3's exit): a message can
 * carry up to this many 64-bit words beyond its registers, from the
 * sender's buffer to the receiver's, one 4 KiB page each. */
#define JOIST_MSG_WORDS 512u

struct joist_msg {
    uint64_t r[JOIST_MSG_REGS];
    /* Capability transfer. Sending: ncaps (at most JOIST_MSG_CAPS)
     * capabilities, named by slot in the sender's space, each needing GRANT
     * and copied with its rights masked by mask[i] (I8). Received: ncaps is
     * the number sent, and cap[i] the receiver's slot capability i landed
     * in, or JOIST_SLOT_NONE if it didn't arrive (the receiver offered no
     * slot for it, the slot was taken or absent, or the sender's capability
     * was gone by delivery); positions past ncaps, and every mask, read 0.
     * A transfer never blocks and never allocates beyond what the
     * receiver's slots already provide. All zero: no capabilities. */
    uint32_t ncaps;
    joist_slot cap[JOIST_MSG_CAPS];
    joist_rights mask[JOIST_MSG_CAPS];
    /* The IPC buffer. Sending: len words (at most JOIST_MSG_WORDS) from the
     * sender's buffer, which must be registered. Received: len the words
     * that arrived in the receiver's buffer; lost set (and len 0) if words
     * were sent and couldn't be placed (the receiver has no buffer, or one
     * of the two isn't mapped as it must be). Like a capability that can't
     * be placed, never an error for either side. */
    uint32_t len;
    uint8_t lost;
};

typedef uint32_t joist_thread;

struct joist_thread_rec {
    uint8_t state;  /* joist_thread_state */
    uint32_t cs;    /* owning capability space (task stand-in) */
    uint32_t ep;    /* while queued: endpoint object index plus one */
    uint32_t qprev; /* queue links: thread index plus one */
    uint32_t qnext;
    uint64_t deadline;    /* JOIST_FOREVER = none */
    uint32_t call_seq;    /* bumped at each Call; a reply capability names (thread, seq) */
    struct joist_msg msg; /* outgoing while BLOCKED_SEND; received after delivery */
    joist_badge badge;    /* while BLOCKED_SEND: the sending capability's badge;
                             after a receive: the badge delivered (I7) */
    joist_err result;     /* outcome of the last blocking operation */
    /* The reply capability this thread holds, if any (I6): the caller's index
     * plus one and the caller's call_seq at delivery. One per thread, as 0001
     * describes, not a capability-space slot. */
    uint32_t reply_to;
    uint32_t reply_seq;
    uint32_t obj; /* the thread object naming this thread, plus one; 0 for fixtures */
    /* Where capabilities arriving in messages to this thread go: position i
     * of a message lands in recv_slot[i], for i < nrecv (joist_recv_slots;
     * in the kernel, the thread's IPC buffer). */
    uint32_t nrecv;
    joist_slot recv_slot[JOIST_MSG_CAPS];
    /* Scheduling (§5.3, §5.6). sc: the context bound to this thread as its
     * own (object index plus one), 0 for a passive thread. run_sc: the one it
     * runs on now: its own, a borrowed one, or 0. Donation: a Call delivered
     * to a passive receiver lends the caller's run_sc to it; sc_to (on the
     * lender) and sc_from (on the borrower) link the two, so a context lent
     * on and on forms a chain, and comes back along it (sched.c). */
    uint32_t sc;
    uint32_t run_sc;
    uint32_t sc_from;
    uint32_t sc_to;
    /* Its place among ready threads of its priority: lower runs first. Set
     * from k->ready_seq whenever it becomes ready or uses up a budget, so
     * equal priorities take turns. */
    uint64_t ready_seq;
    struct joist_regs regs;
    /* Faults (0009): the handler, a slot in this thread's own space plus one
     * (looked up when a fault happens; 0 for none), and whether this thread
     * is blocked in a fault call now. */
    uint32_t fault_slot;
    uint8_t faulting;
    /* Its IPC buffer: a page-aligned user address, 0 for none. */
    uint64_t ipc_buf;
    /* The run queues (choice 9, Phase 3 step 10): the queue it is on (a CPU,
     * plus one; 0 none), that list's priority, its neighbours in it (thread
     * plus one), and the CPU whose queue it goes on when it next waits for
     * one (the one it last ran on). On a queue exactly when it can run and
     * is on no CPU (I14). */
    uint8_t rq_on;
    uint8_t home;
    uint8_t rq_prio;  /* the priority whose list it is on */
    uint8_t rq_dirty; /* noted, to be filed when the hold ends (k->rq_hold) */
    uint32_t rq_prev;
    uint32_t rq_next;
    /* The CPU this thread is on, plus one (0: none): the one that last
     * chose it and hasn't chosen another since. A thread is on at most one
     * CPU, and no other CPU chooses it meanwhile (I13; step 9). */
    uint8_t cpu;
};

struct joist_cspace_state {
    uint8_t live;
    uint32_t quota;    /* objects this space may still create */
    uint32_t charged;  /* live objects this space paid for (I3) */
    uint32_t parent;   /* the space that donated this one's quota, plus one;
                          0 for the root and for fixtures */
    uint32_t task_obj; /* the task object naming this space, plus one; 0 if none */
    uint8_t fixture;   /* made by joist_cspace_create: nameless by design */
    uint8_t mcp;       /* the highest priority this space may give a scheduling
                          context (seL4's MCP); a child starts with its parent's */
    uint32_t fault_slot; /* the task's fault handler, for threads without their own:
                            a slot in this space plus one, 0 for none (0009) */
    /* Destruction in progress (decision 0016): the space is still live, but
     * acts as dead to every operation and accepts no capability (TLA+'s
     * rule); its threads are gone, and its slots are being swept from
     * `sweep` upwards, a few per step. */
    uint8_t dying;
    uint32_t sweep;
    /* Its page tables (0020): the top level of the tree. */
    uint64_t pt_top[JOIST_PT_TOP];
    /* Memory lent for them (0021): the lent frames that still have pages
     * never taken, object plus one, linked through lent_next (a frame leaves
     * when full; all lent frames carry `lent`); a free list of pages given
     * back by emptied tables, page plus one, linked through each page's
     * first word; how many pages it can still take, and how many its tables
     * use. */
    uint32_t lent_first;
    uint64_t pt_free;
    uint64_t pt_avail;
    uint64_t pt_used;
    /* Dying, its slots swept: the next last-level table to sweep (its first
     * virtual page / JOIST_PT_ENTRIES). */
    uint64_t tsweep;
    /* Leaf 0: slots 0 .. JOIST_CNODE_LEAF - 1. */
    struct joist_cap slot[JOIST_CNODE_LEAF];
    /* The other leaves: leaf[i] is the pool index plus one of leaf i, 0 if
     * absent. leaf[0] is unused (leaf 0 is slot[] above). */
    uint32_t leaf[JOIST_CNODE_ROOT];
};

/* A leaf of the capability table, from the pool (k->leaf). */
struct joist_leaf {
    uint32_t owner; /* the space using it, plus one; 0 = free */
    uint32_t index; /* which leaf of that space it is (1 .. JOIST_CNODE_ROOT - 1) */
    struct joist_cap slot[JOIST_CNODE_LEAF];
};

struct joist_kernel {
    struct joist_object obj[JOIST_MAX_OBJECTS];
    struct joist_cspace_state cs[JOIST_MAX_CSPACES];
    struct joist_thread_rec th[JOIST_MAX_THREADS];
    struct joist_badge_acct acct[JOIST_MAX_BADGE_ACCTS];
    struct joist_deadname dn[JOIST_MAX_DEADNAMES];
    struct joist_leaf leaf[JOIST_MAX_LEAVES];
    uint32_t objects_live;
    /* Free objects (Phase 4, choice 10), found in O(1): every object at or
     * above obj_high has never been used; below it the free ones form a
     * list from obj_free (an index plus one; 0, none) through obj_next, the
     * last freed first. Kept apart from the objects, so a free one stays
     * all zero. */
    uint32_t obj_high;
    uint32_t obj_free;
    uint32_t obj_next[JOIST_MAX_OBJECTS];
    /* Frames dying (0020) or killed (0022) while still mapped, and the
     * sweep that unmaps them: the next (space, last-level table) to look at, as
     * space * JOIST_PT_SWEEP_SPACE + table. fround: whether the sweep's
     * round has cleared anything, or a frame began dying, since it began;
     * a round with neither means frames_dying is wrong, and the sweep stops. */
    uint32_t frames_dying;
    uint64_t fsweep;
    uint8_t fround;
    uint64_t now; /* model time: advanced only by joist_tick */
    uint64_t ready_seq; /* the last place given out among ready threads */
    /* While nonzero, joist__rq_sync only notes the threads to file (each
     * once, in rq_pend; rq_dirty in the record) and joist__rq_settle files
     * them: a path that changes a thread several times (a Call's rendezvous:
     * woken, lent a context, given a place) moves it once. Zero between
     * operations, with nothing noted (I14). */
    uint32_t rq_hold;
    uint32_t rq_npend;
    /* k->now at the last joist_tick: a context whose budget is spent can run
     * again from the first tick at or after its refill time (the kernel's
     * timer is set for it). */
    uint64_t tick_now;
    /* Steps taken past a list's end to insert a thread in its place (a
     * thread taking an older place than the last's): counted, since the
     * queues are O(1) only where these are none. */
    uint64_t rq_walks;
    /* The thread on each CPU, plus one (0: none); the other side of every
     * thread's `cpu` (I13). */
    uint32_t on_cpu[JOIST_MAX_CPUS];
    /* Set by any operation after which some CPU other than the one acting
     * may deserve a different thread than it has (a thread became able to
     * run, or one that runs no longer may): the kernel then asks
     * joist_sched_better for each and interrupts those that do, and clears
     * it. Never wrong when clear (test_smp.c holds it to that). */
    uint8_t resched;
    /* Quota conservation (I3): every unit of quota entered through joist_boot
     * or a fixture space, and
     *   sum over live spaces of quota + sum over all spaces of charged == budget.
     * Quota only leaves when a parentless (fixture) space or its charges die. */
    uint64_t budget;
    uint32_t root; /* the root space, plus one, once booted (0013) */
    /* Dying spaces still being swept, the newest on top: destroying a task
     * can make others unreachable, and they are destroyed by a loop, not by
     * recursion. Every dying space is here, and only those (0016). */
    uint32_t pending[JOIST_MAX_CSPACES];
    uint32_t npending;
    /* Preemption (0016): the most slots one operation sweeps before it
     * returns and leaves the rest to joist_continue; 0 for no limit. The
     * kernel sets it from whether an interrupt is pending; tests set it to
     * interleave destruction with everything else. */
    uint32_t preempt;
    /* Loop iterations run by operations so far (I9; see JOIST_WORK). */
    uint64_t work;
    struct joist_hooks hooks; /* the kernel's; zero in the host model */
    /* (Last: large, and apart from the fields every operation uses.)
     * The run queues, one per CPU (choice 9): for each priority, a list of
     * the threads that can run and are on no CPU, in their places in line
     * (ready_seq, oldest first), and a bitmap of the priorities whose list
     * isn't empty. Which CPU's queue a thread is on decides nothing: each
     * CPU chooses from the best head of every queue and its own thread, so
     * the choice is the scan's (joist_sched_pick_on), in a time bounded by
     * the number of CPUs, not of threads. */
    struct joist_runq {
        uint64_t bits[JOIST_PRIO_WORDS];
        struct {
            uint32_t head, tail; /* thread plus one; together, one cache line */
        } list[JOIST_PRIOS];
    } rq[JOIST_MAX_CPUS];
    uint32_t rq_pend[JOIST_MAX_THREADS]; /* (see rq_hold) */
};

/* Lifecycle. */
void joist_init(struct joist_kernel *k);

/* Create a capability space with an object quota, from nothing (the quota
 * is added to the budget). A test fixture: real spaces are tasks, created by
 * joist_task_create from donated quota. JOIST_E_NOMEM if the budget would
 * pass UINT32_MAX: every quota is a share of the budget, held in 32 bits. */
joist_err joist_cspace_create(struct joist_kernel *k, uint32_t quota, joist_cspace *out);

/* Delete every capability in the space, then mark it dead (I1 end to end).
 * Bounded by the space's leaves times JOIST_CNODE_LEAF. */
joist_err joist_cspace_destroy(struct joist_kernel *k, joist_cspace cs);

/* Add leaf `index` (1 .. JOIST_CNODE_ROOT - 1) to cs's capability table, so
 * slots index * JOIST_CNODE_LEAF .. + JOIST_CNODE_LEAF - 1 exist. Charged to
 * cs's quota (one unit) and taken from the leaf pool. JOIST_E_RANGE (index
 * 0 or past the root), JOIST_E_OCCUPIED (already there), JOIST_E_NOMEM
 * (quota or pool), JOIST_E_DEAD. Bounded by JOIST_MAX_LEAVES. */
joist_err joist_cspace_grow(struct joist_kernel *k, joist_cspace cs, uint32_t index);

/* Remove leaf `index` from cs's table, refunding its unit. It must be empty
 * (JOIST_E_STATE otherwise): a leaf is never removed with capabilities in it.
 * JOIST_E_RANGE (index 0, past the root, or not there), JOIST_E_DEAD. O(leaf). */
joist_err joist_cspace_shrink(struct joist_kernel *k, joist_cspace cs, uint32_t index);

/* Create an object of the given type, charged to cs's quota, and put a
 * capability with all rights into slot dst. */
joist_err joist_obj_create(struct joist_kernel *k, joist_cspace cs, joist_obj_type type,
                           joist_slot dst);

/* Copy the capability in src to dst within cs, keeping only the rights in
 * mask (I8: rights never grow). The badge is copied unchanged. */
joist_err joist_cap_derive(struct joist_kernel *k, joist_cspace cs, joist_slot src, joist_slot dst,
                           joist_rights mask);

/* Like derive, but also set a badge (I7). Requires GRANT on src, an unbadged
 * src, and a nonzero badge. */
joist_err joist_cap_mint(struct joist_kernel *k, joist_cspace cs, joist_slot src, joist_slot dst,
                         joist_rights mask, joist_badge badge);

/* Give a task a capability: copy the capability in cs's slot src (which
 * needs GRANT) into slot dst of the task named by task_slot (which needs
 * CONTROL), keeping only the rights in mask (I8). How a task's creator
 * hands it its first capabilities (the root task, 0013, setting up the
 * tasks it starts). Results: as for a task capability with CONTROL
 * (JOIST_E_RANGE/JOIST_E_DEAD for cs, then JOIST_E_RANGE, JOIST_E_EMPTY,
 * JOIST_E_TYPE, JOIST_E_RIGHTS, JOIST_E_GONE), then JOIST_E_RANGE (src or
 * dst), JOIST_E_EMPTY (src), JOIST_E_RIGHTS (no GRANT), JOIST_E_OCCUPIED. */
joist_err joist_cap_give(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                         joist_slot src, joist_slot dst, joist_rights mask);

/* Delete the capability in slot. Deleting the last capability to an object
 * destroys it and refunds its donor. */
joist_err joist_cap_delete(struct joist_kernel *k, joist_cspace cs, joist_slot slot);

/* Capabilities one joist_cap_copy_batch copies at most: the size of one
 * destruction step (decision 0022; the kernel's RUN_PREEMPT_SLOTS), and the
 * width of its skipped bitmap. */
#define JOIST_COPY_BATCH 32u
/* joist_cap_copy_batch's flags: copy receive rights (an endpoint capability
 * with RECV) too. Mach's fork does not inherit them; glibc makes new ones. */
#define JOIST_COPY_RECV 1u

/* The batched capability copy for fork (decision 0022, 0011): for each
 * occupied slot s in [first, first + count) of cs, a copy in slot s of the
 * task named by task_slot (which needs CONTROL), with the rights in mask
 * (I8) and the same badge. Capabilities without GRANT are skipped, and so
 * are copies that would carry RECV on an endpoint, unless flags has
 * JOIST_COPY_RECV; bit i of *skipped is slot first + i skipped. It stops at
 * the first slot whose copy would land on an occupied slot of the task
 * (JOIST_E_OCCUPIED) or in a leaf the task doesn't have (JOIST_E_RANGE: the
 * caller grows it first and pays for it), with *done the slots dealt with
 * before it (count on success): those copies stay, and the caller goes on
 * from first + *done. Results: JOIST_E_RANGE/JOIST_E_DEAD (cs), then for the
 * task capability JOIST_E_RANGE, JOIST_E_EMPTY, JOIST_E_TYPE, JOIST_E_RIGHTS
 * (CONTROL), JOIST_E_DEAD (the task is dying or destroyed: TLA+'s rule, a
 * dying space accepts no capability), then JOIST_E_RANGE (count over
 * JOIST_COPY_BATCH, the range past the table, or an unknown flag), then the
 * stops. Never allocates: a copy only counts a reference, and the no-senders
 * account of any badge copied exists already (its source is counted in it).
 * Atomic per call only. Bounded by JOIST_COPY_BATCH copies. */
joist_err joist_cap_copy_batch(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                               joist_slot first, uint32_t count, joist_rights mask,
                               uint32_t flags, uint32_t *done, uint32_t *skipped);

/* Revoke by kill (decision 0022): the object the capability in slot names
 * dies, and from then on every capability to it, in any space, fails with
 * JOIST_E_GONE wherever it is used. The slots stay occupied until their
 * holders delete them (I1), and the object's quota unit returns to its
 * donor with its last capability, as for any object. Frames only for now
 * (regions to come): a killed frame is unmapped everywhere and can't be
 * mapped or split again. Results: JOIST_E_RANGE/JOIST_E_DEAD (space),
 * JOIST_E_RANGE, JOIST_E_EMPTY, JOIST_E_TYPE (not a type that can be
 * killed), JOIST_E_RIGHTS (no JOIST_R_KILL), JOIST_E_GONE (dead already).
 * (JOIST_E_STATE, a frame lent to a space, comes with 0021.) O(1) but for
 * the unmapping (0020's sweep will make that preemptible). */
joist_err joist_obj_kill(struct joist_kernel *k, joist_cspace cs, joist_slot slot);

/* ---- threads and IPC (Phase 1 work item 2; I6, I7, I10) ----
 *
 * A thread invokes operations only while RUNNING. A blocking operation either
 * completes at once (JOIST_OK, or an error) or returns JOIST_BLOCKED; the
 * thread is then blocked, and when it wakes (rendezvous, timeout, peer death)
 * its record holds the result, message and badge. timeout is relative to
 * k->now; 0 means "do not block", JOIST_FOREVER means "no deadline". */

/* Create a running thread owned by cs. A fixture until work item 4. */
joist_err joist_thread_create(struct joist_kernel *k, joist_cspace cs, joist_thread *out);

/* Destroy a thread: dequeue it, and wake its call peer with JOIST_E_PEER if it
 * held a reply capability. Bounded. */
joist_err joist_thread_destroy(struct joist_kernel *k, joist_thread t);

/* Send m on the endpoint in slot and wait for the reply (there is no bare
 * Send: 0001's 2026-10-03 amendment). timeout covers the whole call; 0 is
 * refused, since a Call always waits. */
joist_err joist_call(struct joist_kernel *k, joist_thread t, joist_slot ep,
                     const struct joist_msg *m, uint64_t timeout);
joist_err joist_recv(struct joist_kernel *k, joist_thread t, joist_slot ep, uint64_t timeout);

/* Name the slots in t's space that capabilities arriving in messages to t go
 * to: position i of a message into slots[i], for i < n (0 <= n <=
 * JOIST_MSG_CAPS; 0: accept none). Kept until changed. The slots are only
 * checked at delivery, where a bad or occupied one just means that
 * capability doesn't arrive. JOIST_E_RANGE (thread, or n too big),
 * JOIST_E_STATE (not running). O(1). */
/* Thread t registers its IPC buffer: a page-aligned address in the user
 * half (JOIST_USER_PAGES), or 0 for none. Checked again only when a
 * message is copied (the hook). JOIST_E_RANGE (not aligned, or past the
 * user half), JOIST_E_RANGE/JOIST_E_STATE (thread). O(1). */
joist_err joist_ipc_buffer(struct joist_kernel *k, joist_thread t, uint64_t va);

joist_err joist_recv_slots(struct joist_kernel *k, joist_thread t, const joist_slot *slots,
                           uint32_t n);
joist_err joist_reply(struct joist_kernel *k, joist_thread t, const struct joist_msg *m);
/* Reply if a reply capability is held (a caller that stopped waiting is not
 * an error here), then Recv. */
joist_err joist_reply_recv(struct joist_kernel *k, joist_thread t, const struct joist_msg *m,
                           joist_slot ep, uint64_t timeout);

/* Deferred replies. A server that can't answer a call at once (proc_wait
 * until a child exits, select until a descriptor is ready, a terminal read
 * until input arrives) saves the reply capability into a reply object it
 * created beforehand (joist_obj_create, JOIST_OBJ_REPLY: paid from its own
 * quota, so nothing is allocated when the call arrives), receives other
 * calls, and replies later through the object, from any of its threads.
 * Single-use as before (I6); the object is reusable afterwards. Deleting the
 * object's last capability while it is bound wakes the caller with
 * JOIST_E_PEER (I10). */

/* Move t's reply capability into the unbound reply object in slot (needs
 * SEND). JOIST_E_NOREPLY if t holds none, JOIST_E_OCCUPIED if the object is
 * already bound. */
joist_err joist_reply_save(struct joist_kernel *k, joist_thread t, joist_slot slot);

/* Reply through the reply object in slot (needs SEND), unbinding it.
 * JOIST_E_NOREPLY if it is unbound, JOIST_E_PEER if the caller stopped
 * waiting. */
joist_err joist_reply_saved(struct joist_kernel *k, joist_thread t, joist_slot slot,
                            const struct joist_msg *m);

/* Advance model time and expire deadlines (JOIST_E_TIMEOUT). Bounded by
 * JOIST_MAX_THREADS. */
void joist_tick(struct joist_kernel *k, uint64_t dt);

/* ---- notifications and lifetime events (Phase 1 work item 3; I2, I4, I5) ---- */

/* OR the badge of the notification capability in slot into the word, or
 * deliver it to the first waiter. Needs SEND and a badged capability (the
 * badge is the signal: unforgeable, I7). Never blocks. */
joist_err joist_signal(struct joist_kernel *k, joist_thread t, joist_slot slot);

/* Return and clear the word (in the thread's badge field), or block until a
 * signal. Needs RECV. Bits are never lost or delivered twice (I2). */
joist_err joist_wait(struct joist_kernel *k, joist_thread t, joist_slot slot, uint64_t timeout);

/* Bind a notification to an endpoint for no-senders (I4): when the last
 * send-capable capability carrying badge B is deleted, B is queued on the
 * endpoint and the notification is signalled with the badge of the
 * capability in note_slot (a summary bit the owner chose: badges are 64-bit
 * values, too many to be bits themselves). ep_slot needs RECV (the
 * endpoint's owner); note_slot needs SEND and a badge. Rebinding replaces the
 * binding; badges already queued stay queued. */
joist_err joist_bind_nosenders(struct joist_kernel *k, joist_cspace cs, joist_slot ep_slot,
                               joist_slot note_slot);

/* Collect one queued no-senders badge from the endpoint in ep_slot (needs
 * RECV), oldest first, into *badge, freeing its account; *badge = 0 (never a
 * valid badge) when none is queued. Bounded: O(1). */
joist_err joist_nosenders_next(struct joist_kernel *k, joist_cspace cs, joist_slot ep_slot,
                               joist_badge *badge);

/* Ask to be told when the endpoint named by ep_slot dies (I5): the
 * notification in note_slot is signalled once with its capability's badge.
 * Needs SEND on note_slot; any capability to the endpoint will do. */
joist_err joist_request_deadname(struct joist_kernel *k, joist_cspace cs, joist_slot ep_slot,
                                 joist_slot note_slot);

/* ---- tasks and threads (Phase 1 work item 4; I1, I3; 0004, 0010, 0013) ---- */

/* Boot (0013): create the root space holding the whole budget. The root has
 * no task object and can't be destroyed. k must be freshly initialised. */
joist_err joist_boot(struct joist_kernel *k, uint32_t budget, joist_cspace *root);

/* Create a task: a new space whose quota is donated from cs's (cs pays
 * donation + 1, the one being the task object), and a task capability with
 * all rights in cs's slot dst. The new space's parent is cs. */
joist_err joist_task_create(struct joist_kernel *k, joist_cspace cs, uint32_t donation,
                            joist_slot dst);

/* Destroy the task named by the task capability in slot (needs CONTROL):
 * its threads, its capabilities, and, through them, any task this makes
 * unreachable. Its unspent quota, and the charges for objects it paid for
 * that are still alive, go to its parent; its children are re-parented to
 * its parent (I3). The task object stays, dead, until its capabilities go.
 * JOIST_E_RIGHTS for the root's own (joist_boot_self): it is never
 * destroyed (0013). */
joist_err joist_task_destroy(struct joist_kernel *k, joist_cspace cs, joist_slot slot);

/* Parent authority (0004 amendment, 2026-10-05). A task may destroy any task
 * it is an ancestor of, by task id (its space index), without a capability
 * to it: the parent chain records who donated its memory. This is what
 * reclaims tasks that hold each other's capabilities in a cycle, which
 * reference counting can't. Destroys exactly as joist_task_destroy.
 * JOIST_E_RANGE (target out of range), JOIST_E_DEAD (already destroyed),
 * JOIST_E_RIGHTS (not a strict descendant: itself, the root and fixtures
 * never are). */
joist_err joist_task_reap(struct joist_kernel *k, joist_cspace cs, joist_cspace target);

/* Continue pending destruction (0016): sweep at most `budget` slots (0: no
 * limit). JOIST_OK when nothing is left, JOIST_BLOCKED if more remains. The
 * kernel calls it when the thread that started a destruction runs again;
 * between calls, every other operation sees dying spaces as dead. */
joist_err joist_continue(struct joist_kernel *k, uint32_t budget);

/* Destruction work left for joist_continue: a dying space still being swept,
 * or a frame dying or killed while still mapped (0020, 0022). */
static inline int joist_destruction_pending(const struct joist_kernel *k)
{
    return k->npending != 0 || k->frames_dying != 0;
}

/* The first live strict descendant of cs whose id is >= from, in *out, or
 * JOIST_E_EMPTY if there is none: how an ancestor finds a task it holds no
 * capability to. from may be JOIST_MAX_CSPACES (then always E_EMPTY).
 * Bounded by JOIST_MAX_CSPACES squared. */
joist_err joist_task_descendant(struct joist_kernel *k, joist_cspace cs, joist_cspace from,
                                joist_cspace *out);

/* Create a running thread in the task named by task_slot (needs CONTROL),
 * charged to cs, with a thread capability with all rights in cs's slot dst.
 * The thread's index is written to *out. */
joist_err joist_thread_spawn(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                             joist_slot dst, joist_thread *out);

/* Kill the thread named by the thread capability in slot (needs CONTROL). */
joist_err joist_thread_kill(struct joist_kernel *k, joist_cspace cs, joist_slot slot);

/* ---- scheduling contexts and donation (Phase 3 step 3; I11) ---- */

/* Create a scheduling context with priority prio (at most cs's mcp:
 * JOIST_E_RIGHTS), and budget <= period, both nonzero (JOIST_E_RANGE),
 * charged to cs, with a capability with all rights in dst. */
joist_err joist_sc_create(struct joist_kernel *k, joist_cspace cs, joist_slot dst, uint8_t prio,
                          uint64_t budget, uint64_t period);

/* Bind the scheduling context in sc_slot to the thread in th_slot as its own
 * (both need CONTROL). JOIST_E_OCCUPIED if either is already bound. A thread
 * not holding a borrowed context starts running on it. */
joist_err joist_sc_bind(struct joist_kernel *k, joist_cspace cs, joist_slot sc_slot,
                        joist_slot th_slot);

/* Unbind the scheduling context in sc_slot from its thread (CONTROL), which
 * becomes passive. JOIST_E_STATE if it is unbound or lent out right now. */
joist_err joist_sc_unbind(struct joist_kernel *k, joist_cspace cs, joist_slot sc_slot);

/* ---- scheduling (Phase 3 step 5; §5.6): who runs, and until when ----
 * Fixed priorities, 0 to JOIST_PRIO_MAX. A thread can run if it is running
 * (not blocked or suspended), runs on some context (its own or a borrowed
 * one), and that context has budget left in its current period or is due a
 * new one. The thread to run is the one of highest priority that can, and
 * among equals the one that has waited longest. These are the scheduler's
 * rules, as plain bounded scans of the thread table; the kernel calls them
 * on every entry. */

/* The thread that should run at time k->now, in *t (JOIST_OK), starting a
 * new period for its context if one is due; JOIST_E_EMPTY if none can.
 * Bounded by JOIST_MAX_THREADS. */
joist_err joist_sched_pick(struct joist_kernel *k, joist_thread *t);

/* Thread t ran for `used` time units: charged to the context it runs on. If
 * that uses the budget up, t goes behind its equals. No effect if t runs on
 * no context. */
void joist_sched_charge(struct joist_kernel *k, joist_thread t, uint64_t used);

/* Yield (decision 0014): thread t gives up the rest of its turn and goes
 * behind its equals; its budget is untouched, and a thread of lower
 * priority still doesn't run before it. JOIST_E_RANGE/JOIST_E_STATE (t not
 * a running thread). O(1). */
joist_err joist_yield(struct joist_kernel *k, joist_thread t);

/* ---- more than one CPU (Phase 3 step 9; decision 0016) ----
 *
 * Scheduling is global: any CPU may run any thread, one CPU per thread
 * (I13). The kernel runs these under its one lock. */

/* joist_sched_pick for CPU `cpu`: the best thread that can run and is on no
 * other CPU. It is then on this CPU (and whatever was, isn't).
 * JOIST_E_EMPTY: nothing for this CPU, which is then idle; JOIST_E_RANGE.
 * joist_sched_pick(k, t) is this for CPU 0. */
joist_err joist_sched_pick_on(struct joist_kernel *k, uint32_t cpu, joist_thread *t);

/* Would joist_sched_pick_on(k, cpu) choose another thread than the one on
 * that CPU now (or one, if it is idle)? Changes nothing. For deciding which
 * other CPUs to interrupt after an operation set k->resched. */
int joist_sched_better(const struct joist_kernel *k, uint32_t cpu);

/* Which CPUs to interrupt, as a mask (decision 0018), after CPU `me` has
 * chosen: of `ncpus`, those in `inflight` have been interrupted already
 * and will choose before anything else. Each thread on the queues that
 * some CPU should now run is left to one CPU only: first to one in
 * `inflight` that would take it, then to an idle CPU, lowest first, then
 * to a busy one, the one running the thread that goes last first, each
 * choosing as joist_sched_pick_on would
 * with the threads already left to others taken out. A CPU whose thread
 * can't run any more is in the mask too. So a thread made ready with
 * several CPUs idle wakes the lowest of them, and only it; and whatever
 * order the interrupted CPUs then choose in, no thread that should run is
 * left waiting (test_smp.c), provided the kernel asks again after each
 * choice made while any are on their way. O(CPUs x (CPUs + priority
 * words)), but the scan while the queues are held with something noted. */
uint32_t joist_sched_pokes(const struct joist_kernel *k, uint32_t me, uint32_t inflight,
                           uint32_t ncpus);

/* Thread t, the one on CPU `cpu`, has made a call and is still there: may
 * it simply go on, without the scheduler choosing again and with the timer
 * as the kernel last set it? Only if the scheduler would choose t again and
 * change nothing in choosing it (t can run, no new period is due, no thread
 * is better for this CPU), and no CPU is asked to choose again (k->resched
 * clear: the kernel then pokes none). k->resched also stands for the next
 * event: a call that leaves its caller running can bring that sooner only
 * by making some thread ready, or by binding, freeing or taking back a
 * context, and each of those sets k->resched. (A thread blocking with a
 * timeout is the caller itself, which then isn't running.) So when t stays,
 * joist_sched_pick_on would choose t and change nothing, and
 * joist_sched_next_event(t) is no sooner than when the timer was set, but
 * for what other CPUs made meanwhile, which each covers with its own timer.
 * test_runq.c holds it to all three. O(CPUs). */
int joist_sched_stays(const struct joist_kernel *k, uint32_t cpu, joist_thread t);

/* Hold the run queues across several operations (they nest): until the
 * matching joist_sched_settle, a thread whose state changes is only noted,
 * and the settle files each noted thread once, where it then belongs. The
 * kernel holds them for a whole system call, through the direct switch
 * (joist_sched_on), so that a thread handed the CPU never goes on a queue
 * and off it again. A choice made while they are held (pick, better) files
 * what is noted first, or scans. Never held between kernel entries (I14). */
void joist__sched_flush(struct joist_kernel *k); /* (files what is noted) */
static inline void joist_sched_hold(struct joist_kernel *k)
{
    k->rq_hold++;
}
static inline void joist_sched_settle(struct joist_kernel *k)
{
    if (k->rq_hold > 1u) {
        k->rq_hold--; /* an inner hold: the outer one settles */
        return;
    }
    k->rq_hold = 0;
    if (k->rq_npend != 0) {
        joist__sched_flush(k);
    }
}

/* CPU `cpu` goes on with thread t without asking (the direct switch,
 * below), or with none (t = JOIST_MAX_THREADS): the record of who is where
 * follows. */
void joist_sched_on(struct joist_kernel *k, uint32_t cpu, joist_thread t);

/* The direct switch (Phase 3 step 8: the fast path's scheduling decision).
 * Thread `from` was joist_sched_pick's choice at k->now, in place `place`
 * among its equals (its ready_seq then), with nothing due before k->now
 * (joist_sched_next_event); it then made a Call or a ReplyRecv that may have
 * handed the context it ran on to thread `to` (the receiver of the call;
 * the caller replied to). Returns `to` if that is so and it is certain that
 * joist_sched_pick would now choose `to`, and that the next event is no
 * earlier than it was: the kernel may run `to` at once, the timer as it is.
 * JOIST_MAX_THREADS otherwise: ask the scheduler. Never wrong, only
 * sometimes unsure. O(1). */
joist_thread joist_sched_handed(const struct joist_kernel *k, joist_thread from, uint64_t place,
                                joist_thread to);

/* The next time something changes without a system call, given that thread
 * `running` runs from k->now (JOIST_MAX_THREADS: nobody does): its budget
 * running out, a blocked thread's deadline, or a new period for a thread
 * waiting on one. JOIST_FOREVER if nothing will. For the one-shot timer.
 * Bounded by JOIST_MAX_THREADS. */
uint64_t joist_sched_next_event(const struct joist_kernel *k, joist_thread running);

/* ---- frames and mappings (Phase 3 step 3, Phase 4 step 2; 0013, 0020,
 * 0021; I12) ----
 * The kernel manages only page tables and frame capabilities (§5.5): it
 * creates frames for free memory at boot, held by the root task, which
 * splits them, lends them to spaces for page tables and maps them. Every
 * call over pages covers at most one JOIST_MAP_SPAN-aligned run and needs a
 * capability with CONTROL to the task whose space it changes, the caller's
 * own included. No fault is resolved by the kernel. */

/* Boot: a frame of pages [base, base + npages) in the root's slot dst,
 * paid from the root's quota. JOIST_E_STATE if not the root or the range
 * overlaps an existing frame, JOIST_E_RANGE if empty, wrapping or larger
 * than JOIST_FRAME_PAGES_MAX, then as obj_create. */
joist_err joist_boot_frame(struct joist_kernel *k, joist_cspace cs, joist_slot dst, uint64_t base,
                           uint64_t npages);

/* Boot: a task capability with CONTROL to the root's own space, in the
 * root's slot dst, paid from its quota: how the root maps into itself.
 * JOIST_E_STATE if not the root or it has one already, then as obj_create. */
joist_err joist_boot_self(struct joist_kernel *k, joist_cspace cs, joist_slot dst);

/* Split the frame in slot (CONTROL) at page `at`: it keeps [base, base +
 * at), and a new frame with the rest goes in dst with the same rights,
 * paid by cs. JOIST_E_RANGE (at not inside), JOIST_E_OCCUPIED (dst),
 * JOIST_E_STATE (mapped, lent, or another capability names it),
 * JOIST_E_NOMEM. */
joist_err joist_frame_split(struct joist_kernel *k, joist_cspace cs, joist_slot slot, uint64_t at,
                            joist_slot dst);

/* Lend the frame in frame_slot (CONTROL) to the space of the task in
 * task_slot (CONTROL) for its page tables (0021): from now on it can't be
 * mapped or split, and when the space dies it is an ordinary frame again.
 * JOIST_E_STATE (mapped, lent, dying; or pages the core can't reach: no
 * page hook, its first or last page unreachable, or beyond
 * JOIST_PT_PFN_LIMIT), after the capabilities' errors. O(1). */
joist_err joist_space_donate(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                             joist_slot frame_slot);

/* Map pages [off, off + npages) of the frame in frame_slot (CONTROL;
 * JOIST_R_WRITE too for JOIST_MAP_W) at virtual page va of the task in
 * task_slot (CONTROL). JOIST_E_RIGHTS (W and X together, an unknown
 * permission, or W without JOIST_R_WRITE), JOIST_E_RANGE (npages 0, beyond
 * the frame, outside the user half, or not within one JOIST_MAP_SPAN),
 * JOIST_E_STATE (the frame lent or dying), JOIST_E_OCCUPIED (a page already
 * mapped), JOIST_E_NOMEM (too few lent pages for the tables it needs). */
joist_err joist_map(struct joist_kernel *k, joist_cspace cs, joist_slot frame_slot, uint64_t off,
                    uint64_t npages, joist_slot task_slot, uint64_t va, unsigned perms);

/* As joist_map, but every page of the run must already be mapped, to pages
 * [old_off, old_off + npages) of the frame in old_slot (CONTROL), and is
 * replaced: JOIST_E_STATE if any page holds anything else, and nothing
 * changes (0020's conditional replace). */
joist_err joist_map_replace(struct joist_kernel *k, joist_cspace cs, joist_slot frame_slot,
                            uint64_t off, uint64_t npages, joist_slot task_slot, uint64_t va,
                            unsigned perms, joist_slot old_slot, uint64_t old_off);

/* Unmap whatever is mapped in [va, va + npages) of the task in task_slot
 * (CONTROL); holes are fine. Tables left empty go back to the space's lent
 * pages. JOIST_E_RANGE (npages 0, outside the user half or one
 * JOIST_MAP_SPAN). */
joist_err joist_unmap_range(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                            uint64_t va, uint64_t npages);

/* Set the permissions of the pages mapped in [va, va + npages) of the task
 * in task_slot (CONTROL); holes are fine. JOIST_E_RIGHTS (W and X together,
 * an unknown permission, or W on a page whose rights ceiling lacks it: then
 * nothing changes), JOIST_E_RANGE as joist_unmap_range. */
joist_err joist_protect(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot, uint64_t va,
                        uint64_t npages, unsigned perms);

/* What is mapped at [va, va + npages) of the task in task_slot (CONTROL),
 * npages at most JOIST_QUERY_MAX, within one JOIST_MAP_SPAN (0020, amended
 * 2026-10-27): out[i] is 0 for an absent page, or JOIST_QUERY_PRESENT with
 * the page's permissions (JOIST_MAP_W, JOIST_MAP_X), JOIST_QUERY_CEIL_W if
 * its rights ceiling allows writing, and JOIST_QUERY_FRAME with the page
 * offset (JOIST_QUERY_OFF) if it maps the frame in frame_slot (CONTROL;
 * JOIST_SLOT_NONE names none). Never another frame, nor a physical
 * address. O(npages); changes nothing, out included, on an error:
 * the capabilities' errors, then JOIST_E_RANGE (npages 0 or past
 * JOIST_QUERY_MAX, outside the user half or one JOIST_MAP_SPAN). */
#define JOIST_QUERY_MAX 64u
#define JOIST_QUERY_CEIL_W 0x4u
#define JOIST_QUERY_PRESENT 0x8u
#define JOIST_QUERY_FRAME 0x10u
#define JOIST_QUERY_OFF(e) ((e) >> 32)
joist_err joist_map_query(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                          uint64_t va, uint64_t npages, joist_slot frame_slot, uint64_t *out);

/* Set the mcp of the task named by task_slot (CONTROL) to at most cs's own
 * (JOIST_E_RIGHTS otherwise). */
joist_err joist_task_set_mcp(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                             uint8_t mcp);

/* ---- thread control and faults (Phase 3 step 3; 0009, 0010) ----
 * Each takes a thread capability with CONTROL in cs's slot th_slot (or a
 * task capability for joist_task_set_fault): JOIST_E_RANGE/JOIST_E_DEAD
 * (space), then JOIST_E_RANGE, JOIST_E_EMPTY, JOIST_E_TYPE, JOIST_E_RIGHTS,
 * JOIST_E_GONE for the capability. */

/* Read or write the thread's register record. */
joist_err joist_thread_read_regs(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot,
                                 struct joist_regs *out);
joist_err joist_thread_write_regs(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot,
                                  const struct joist_regs *in);

/* Cancel the thread's blocking operation, if it is in one: it wakes with
 * JOIST_E_ABORTED (glibc's EINTR, 0010's abort). A caller waiting for its
 * reply stops waiting (the server's reply then gets JOIST_E_PEER). A
 * running or suspended thread is left as it is. */
joist_err joist_thread_abort(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot);

/* Stop the thread (aborting a blocking operation first) / let it run again
 * (JOIST_E_STATE unless suspended). Suspending twice is not an error. */
joist_err joist_thread_suspend(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot);
joist_err joist_thread_resume(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot);

/* Name the fault handler of a thread, or of a task (for its threads without
 * their own): a slot in the thread's or task's own space, looked up when a
 * fault happens, or JOIST_SLOT_NONE for none. Nothing is checked until then. */
joist_err joist_thread_set_fault(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot,
                                 joist_slot handler);
joist_err joist_task_set_fault(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                               joist_slot handler);

/* A fault in running thread t (the kernel's trap handlers call this; the
 * model's tests and executor call it directly). The handler is the thread's,
 * else its task's: an endpoint capability with SEND, alive. With one, the
 * fault is a Call to it (JOIST_BLOCKED; the thread's context is lent as for
 * any Call, so the fault is charged to the faulting thread, 0009); its reply
 * resumes or terminates the thread, and if the call ends any other way
 * (handler gone, aborted) the thread is left suspended. With none, the task
 * is terminated, or, for the root task, which can't be, the thread
 * (JOIST_E_PEER). JOIST_E_RANGE/JOIST_E_STATE (thread not running). */
joist_err joist_fault(struct joist_kernel *k, joist_thread t, uint64_t kind, uint64_t addr,
                      uint64_t code);

/* Read a capability (for tests and the eventual ABI's inspection call). */
joist_err joist_cap_read(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                         struct joist_cap *out);

/* Check the structural invariants over the whole model state. Returns the
 * number of violations; writes a description of the first into msg.
 * O(objects + spaces * slots): a test aid, not a kernel operation. */
unsigned joist_check_invariants(const struct joist_kernel *k, char *msg, unsigned msglen);

const char *joist_err_name(joist_err e);

#endif /* JOIST_CORE_H */
