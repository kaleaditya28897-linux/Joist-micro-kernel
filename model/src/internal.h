/* internal.h - hooks between the model's source files. Not part of the API. */
#ifndef JOIST_MODEL_INTERNAL_H
#define JOIST_MODEL_INTERNAL_H

#include "joist/model.h"

/* An endpoint's last capability is gone: wake every queued thread with
 * JOIST_E_GONE and empty the queue. Bounded by JOIST_MAX_THREADS. */
void joist__endpoint_destroyed(struct joist_kernel *k, uint32_t obj_index);

/* Destroy every thread owned by cs (before its capabilities are deleted). */
void joist__threads_destroy_owned(struct joist_kernel *k, joist_cspace cs);

/* Structural IPC invariants (queues, thread states, reply capabilities).
 * Returns the number of violations; writes the first into msg if msg_free. */
unsigned joist__check_ipc(const struct joist_kernel *k, char *msg, unsigned msglen, int msg_free);

#endif /* JOIST_MODEL_INTERNAL_H */
