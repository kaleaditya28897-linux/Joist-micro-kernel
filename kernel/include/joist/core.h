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
    JOIST_BLOCKED     /* not an error: the thread is now blocked; the outcome is
                         left in its record when it wakes */
} joist_err;

typedef enum {
    JOIST_OBJ_NONE = 0,
    JOIST_OBJ_ENDPOINT,
    JOIST_OBJ_NOTIFICATION,
    JOIST_OBJ_TASK,   /* a capability space with its threads (0004) */
    JOIST_OBJ_THREAD, /* a thread; its capability is 0010's thread control capability */
    JOIST_OBJ_REPLY   /* holds one saved reply capability: a deferred reply (0001) */
} joist_obj_type;

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
     * reaches 0. */
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
};

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
    JOIST_T_BLOCKED_WAIT   /* queued on a notification */
} joist_thread_state;

struct joist_msg {
    uint64_t r[JOIST_MSG_REGS];
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
};

struct joist_cspace_state {
    uint8_t live;
    uint32_t quota;    /* objects this space may still create */
    uint32_t charged;  /* live objects this space paid for (I3) */
    uint32_t parent;   /* the space that donated this one's quota, plus one;
                          0 for the root and for fixtures */
    uint32_t task_obj; /* the task object naming this space, plus one; 0 if none */
    uint8_t fixture;   /* made by joist_cspace_create: nameless by design */
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
    uint64_t now; /* model time: advanced only by joist_tick */
    /* Quota conservation (I3): every unit of quota entered through joist_boot
     * or a fixture space, and
     *   sum over live spaces of quota + sum over all spaces of charged == budget.
     * Quota only leaves when a parentless (fixture) space or its charges die. */
    uint64_t budget;
    uint32_t root; /* the root space, plus one, once booted (0013) */
    /* Spaces whose destruction is pending: a task's destruction can make other
     * tasks unreachable, and they are destroyed by a loop, not by recursion. */
    uint32_t pending[JOIST_MAX_CSPACES];
    uint32_t npending;
    /* Loop iterations run by operations so far (I9; see JOIST_WORK). */
    uint64_t work;
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

/* Delete the capability in slot. Deleting the last capability to an object
 * destroys it and refunds its donor. */
joist_err joist_cap_delete(struct joist_kernel *k, joist_cspace cs, joist_slot slot);

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
 * its parent (I3). The task object stays, dead, until its capabilities go. */
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

/* Read a capability (for tests and the eventual ABI's inspection call). */
joist_err joist_cap_read(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                         struct joist_cap *out);

/* Check the structural invariants over the whole model state. Returns the
 * number of violations; writes a description of the first into msg.
 * O(objects + spaces * slots): a test aid, not a kernel operation. */
unsigned joist_check_invariants(const struct joist_kernel *k, char *msg, unsigned msglen);

const char *joist_err_name(joist_err e);

#endif /* JOIST_CORE_H */
