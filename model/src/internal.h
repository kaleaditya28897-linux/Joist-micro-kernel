/* internal.h - hooks between the model's source files. Not part of the API. */
#ifndef JOIST_MODEL_INTERNAL_H
#define JOIST_MODEL_INTERNAL_H

#include "joist/model.h"

/* I9's host stand-in for WCET: every loop an operation runs counts one unit
 * of work per iteration, so a test can bound each operation's work. The
 * invariant checks don't count; they aren't operations. */
#define JOIST_WORK(k) ((k)->work++)

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

#endif /* JOIST_MODEL_INTERNAL_H */
