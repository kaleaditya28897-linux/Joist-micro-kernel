/* work.h - what the scheduler test's root task (schedroot) and its workers
 * share (Phase 3 step 5): one page each, mapped writable in both, through
 * which the root tells a worker what to be and reads how far it has got. */
#ifndef JOIST_USER_WORK_H
#define JOIST_USER_WORK_H

#include <stdint.h>

#define WORK_VA 0x30000000ull /* a worker's page, in the worker */

enum {
    WORK_SPIN = 1, /* count for ever, never calling the kernel */
    WORK_SERVE,    /* on the endpoint in slot 0: arg counts per request, then the reply */
    WORK_CLIENT    /* call the endpoint in slot 0 for ever; count is the calls answered */
};

struct work {
    volatile uint64_t role;
    volatile uint64_t arg;
    volatile uint64_t count;
};

/* The one loop every count comes from, so counts measure time alike
 * wherever they were made. */
static inline void work_spin(struct work *w, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        w->count = w->count + 1u;
    }
}

#endif /* JOIST_USER_WORK_H */
