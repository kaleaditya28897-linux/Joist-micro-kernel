/* internal.h - hooks between the core's source files. Not part of the API. */
#ifndef JOIST_CORE_INTERNAL_H
#define JOIST_CORE_INTERNAL_H

#include "joist/core.h"

#include <stddef.h>

/* The core builds both on the host (model/'s tests) and freestanding in the
 * kernel, which has no C library: kernel/lib/string.c provides memset. The
 * invariant checkers' messages are formatted with snprintf on the host; in
 * the kernel they keep the bare format text, which still names the
 * invariant and is enough for a debug build that runs the checks. */
#if __STDC_HOSTED__
#include <stdio.h>
#include <string.h>
#define JOIST_FMT(msg, msglen, ...) ((void)snprintf((msg), (msglen), __VA_ARGS__))
#else
void *memset(void *dst, int c, size_t n);
void joist__fmt_bare(char *msg, unsigned msglen, const char *fmt);
#define JOIST_FMT(msg, msglen, ...) joist__fmt_bare((msg), (msglen), JOIST_FMT_FIRST(__VA_ARGS__, 0))
#define JOIST_FMT_FIRST(first, ...) (first)
#endif

/* I9's host stand-in for WCET: every loop an operation runs counts one unit
 * of work per iteration, so a test can bound each operation's work. The
 * invariant checks don't count; they aren't operations. */
#define JOIST_WORK(k) ((k)->work++)

/* The capability in slot of space cs, or NULL if the slot is out of range
 * or its leaf is absent (both are JOIST_E_RANGE to callers). cs must be a
 * valid space index. O(1). */
struct joist_cap *joist__slot(struct joist_kernel *k, uint32_t cs, joist_slot slot);
const struct joist_cap *joist__slot_c(const struct joist_kernel *k, uint32_t cs, joist_slot slot);
/* The JOIST_CNODE_LEAF slots of leaf l of space cs, or NULL if absent: how
 * every walk over a space's capabilities goes (leaf by leaf, skipping absent
 * ones). l < JOIST_CNODE_ROOT. */
struct joist_cap *joist__leaf_slots(struct joist_kernel *k, uint32_t cs, uint32_t l);
const struct joist_cap *joist__leaf_slots_c(const struct joist_kernel *k, uint32_t cs, uint32_t l);

/* Free present, empty leaf `index` (not 0) of space cs, refunding its unit
 * to the space's quota (cspace.c). */
void joist__leaf_free(struct joist_kernel *k, uint32_t cs, uint32_t index);

/* Every capability creation and deletion goes through these two hooks, so
 * the per-endpoint RECV count (endpoint death, I5) and the per-badge send
 * accounting (no-senders, I4) can't drift from the capability tables. A
 * capability added with SEND and a badge on an endpoint needs its accounting
 * entry to exist already (joist__acct_reserve, called by mint). */
void joist__cap_added(struct joist_kernel *k, const struct joist_cap *c);
void joist__cap_removed(struct joist_kernel *k, const struct joist_cap *c);

/* Make sure an accounting entry exists for (obj, badge); allocates one if
 * needed. Only mint calls it, so derive and delete never allocate. */
joist_err joist__acct_reserve(struct joist_kernel *k, uint32_t obj, joist_badge badge);

/* An object's last capability is gone (called just before it is freed). */
void joist__object_freed(struct joist_kernel *k, uint32_t obj);

/* Wake every thread queued on obj with result r, emptying the queue.
 * Bounded by JOIST_MAX_THREADS. */
void joist__queue_wake_all(struct joist_kernel *k, uint32_t obj, joist_err r);

/* Kernel-side signal of a notification (no-senders, dead-name, Signal). */
void joist__notify_signal(struct joist_kernel *k, uint32_t obj, uint64_t bits);

/* Destroy every thread owned by cs (before its capabilities are deleted). */
void joist__threads_destroy_owned(struct joist_kernel *k, joist_cspace cs);

/* Queue helpers shared by IPC and notifications. */
void joist__wake(struct joist_kernel *k, uint32_t t, joist_err result);
void joist__q_push(struct joist_kernel *k, uint32_t obj, joist_queue_kind kind, uint32_t t);
uint32_t joist__q_pop(struct joist_kernel *k, uint32_t obj);
uint64_t joist__deadline(const struct joist_kernel *k, uint64_t timeout);

/* A capability in cs's slot naming a live object of `type`, with CONTROL:
 * JOIST_E_RANGE/JOIST_E_DEAD (space), JOIST_E_RANGE, JOIST_E_EMPTY,
 * JOIST_E_TYPE, JOIST_E_RIGHTS, JOIST_E_GONE (sched.c). */
joist_err joist__control_cap(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                             joist_obj_type type, uint32_t *obj);
/* Cancel t's blocking operation: dequeue it and wake it with result (ipc.c). */
void joist__cancel(struct joist_kernel *k, uint32_t t, joist_err result);
/* A fault in t, as a Call on endpoint obj with that badge (ipc.c). */
joist_err joist__fault_call(struct joist_kernel *k, uint32_t t, uint32_t obj, joist_badge badge,
                            const struct joist_msg *m);

/* Scheduling-context donation (sched.c). */
/* A Call from `from` was delivered to `to`: lend from's context if `to` is
 * passive. */
void joist__sc_donate(struct joist_kernel *k, uint32_t from, uint32_t to);
/* Thread x wakes or dies: take back the context it lent, along the chain. */
void joist__sc_reclaim(struct joist_kernel *k, uint32_t x);
/* Thread h stops serving (receives again, waits, dies): give a borrowed
 * context back to its lender. */
void joist__sc_giveback(struct joist_kernel *k, uint32_t h);
/* Thread t is being destroyed: unwind its donation links, unbind its own
 * context. */
void joist__sc_thread_gone(struct joist_kernel *k, uint32_t t);
/* Bind context object so to thread t (both valid and live): sc_bind's
 * effect once its capabilities are checked; tests use it for fixture
 * threads, which have no thread capability. JOIST_E_OCCUPIED as sc_bind. */
joist_err joist__sc_bind_thread(struct joist_kernel *k, uint32_t so, uint32_t t);
/* A scheduling context's last capability is gone. */
void joist__sc_freed(struct joist_kernel *k, uint32_t obj);
unsigned joist__check_sched(const struct joist_kernel *k, char *msg, unsigned msglen,
                            int msg_free);

/* A reply object's last capability is gone (ipc.c). */
void joist__reply_obj_freed(struct joist_kernel *k, uint32_t obj);

/* Task lifecycle (task.c). */
void joist__space_destroy(struct joist_kernel *k, uint32_t s);
void joist__drain_pending(struct joist_kernel *k);
void joist__lifecycle_freed(struct joist_kernel *k, uint32_t obj);
/* Delete one capability with no argument checks and no draining. */
void joist__cap_delete_slot(struct joist_kernel *k, uint32_t s, joist_slot slot);
/* Copy a capability from one space to another (needs GRANT on the source;
 * rights masked, never grown). A stand-in for capability transfer in
 * messages, which is still open; used only by tests until then. */
joist_err joist__cap_grant(struct joist_kernel *k, joist_cspace from, joist_slot src,
                           joist_cspace to, joist_slot dst, joist_rights mask);
unsigned joist__check_task(const struct joist_kernel *k, char *msg, unsigned msglen, int msg_free);

/* Structural invariants of IPC and of notifications/lifetime events.
 * Each returns the number of violations and writes the first into msg if
 * msg_free. */
unsigned joist__check_ipc(const struct joist_kernel *k, char *msg, unsigned msglen, int msg_free);
unsigned joist__check_notify(const struct joist_kernel *k, char *msg, unsigned msglen,
                             int msg_free);

#endif /* JOIST_CORE_INTERNAL_H */
