/* exec.h - the operation-sequence executor shared by the whole-model property
 * test and the libFuzzer harness (Phase 1 work item 5).
 *
 * One executor decodes operations from a stream of choices, applies them to
 * the model, and checks every invariant after every step. The property test
 * feeds it choices from a seeded RNG; the fuzzer feeds it the fuzz input. Both
 * see the same operations, the same biasing and the same checks, so a fuzzer
 * crash replays in the test runner and vice versa. */
#ifndef JOIST_TEST_EXEC_H
#define JOIST_TEST_EXEC_H

#include "joist/model.h"

/* Where choices come from. next(ctx, n) returns a value in [0, n), n > 0. */
struct jx_source {
    uint32_t (*next)(void *ctx, uint32_t n);
    void *ctx;
};

enum {
    JX_SPACE_CREATE,
    JX_SPACE_DESTROY,
    JX_OBJ_CREATE,
    JX_DERIVE,
    JX_MINT,
    JX_GRANT,
    JX_DELETE,
    JX_TASK_CREATE,
    JX_TASK_DESTROY,
    JX_SPAWN,
    JX_KILL,
    JX_TH_CREATE,
    JX_TH_DESTROY,
    JX_SEND,
    JX_CALL,
    JX_RECV,
    JX_REPLY,
    JX_REPLY_RECV,
    JX_SIGNAL,
    JX_WAIT,
    JX_BIND,
    JX_DEADNAME,
    JX_COLLECT,
    JX_SAVE,
    JX_SAVED,
    JX_TICK,
    JX_OPS
};

#define JX_RESULTS 16u /* result codes counted per operation; the last bucket is BLOCKED */

struct jx_stats {
    unsigned long result[JX_OPS][JX_RESULTS];
    uint64_t max_work[JX_OPS]; /* I9: the most work one call of the operation took */
    unsigned long wakeups[JX_RESULTS];
    unsigned long sequences;
    unsigned long cycles; /* tasks left alive at teardown only by holding each other */
    unsigned long steps;
};

extern const char *const jx_op_name[JX_OPS];

/* I9 bounds on one operation's work (JOIST_WORK units), in closed form from
 * the table sizes, never from anything a caller controls. A sweep is one pass
 * over every table; an operation that frees nothing makes a few. One that
 * can free (delete, destroy, kill) may cascade, and its bound states what the
 * model promises about a cascade: each space is destroyed at most once, each
 * object freed at most once, each capability deleted at most once, each with
 * a fixed cost. The gap between the two bounds is the open preemptible-revoke
 * question (0003). */
#define JX_SWEEP                                                                                   \
    ((uint64_t)JOIST_MAX_OBJECTS + JOIST_MAX_THREADS + JOIST_MAX_CSPACES + JOIST_CSPACE_SLOTS +    \
     JOIST_MAX_BADGE_ACCTS + JOIST_MAX_DEADNAMES)
#define JX_WORK_SIMPLE (4u * JX_SWEEP)
/* Destroying one space: its slots, the children and objects it hands on, its
 * threads, and the work list. */
#define JX_PER_SPACE                                                                               \
    ((uint64_t)JOIST_CSPACE_SLOTS + 2u * JOIST_MAX_CSPACES + JOIST_MAX_OBJECTS + JOIST_MAX_THREADS)
/* Freeing one object: its waiters, bindings to it, its registrations. */
#define JX_PER_FREE                                                                                \
    ((uint64_t)JOIST_MAX_THREADS + JOIST_MAX_OBJECTS + 3u * JOIST_MAX_DEADNAMES +                  \
     JOIST_MAX_BADGE_ACCTS)
/* Deleting one capability: its no-senders account. */
#define JX_PER_CAP ((uint64_t)JOIST_MAX_BADGE_ACCTS)
#define JX_WORK_CASCADE                                                                            \
    ((uint64_t)JOIST_MAX_CSPACES * JX_PER_SPACE + (uint64_t)JOIST_MAX_OBJECTS * JX_PER_FREE +      \
     (uint64_t)JOIST_MAX_CSPACES * JOIST_CSPACE_SLOTS * JX_PER_CAP)

/* Can this operation free objects, and so cascade? */
int jx_op_cascades(unsigned op);

/* Boot a fresh model with a budget chosen from src, and a small starting
 * world in the root (exec.c). */
void jx_begin(struct joist_kernel *k, struct jx_source *src);

/* Choose and apply one operation, then check everything. Returns 1 if all
 * checks passed; otherwise writes the first violation to why and returns 0. */
int jx_step(struct joist_kernel *k, struct jx_source *src, struct jx_stats *st, char *why,
            unsigned whylen);

/* Destroy everything and check that nothing is left: no object, thread,
 * account or registration, and the budget back in the root (I1, I3), with no
 * thread blocked except on a reply its live server still owes (I10).
 * Fixtures are destroyed and the root drops its capabilities, so every task
 * should go by cascade alone. Tasks that survive that hold each other's
 * capabilities in a cycle, which reference counting can't collect: they are
 * counted in st->cycles, then destroyed directly. Returns 1 if clean. */
int jx_teardown(struct joist_kernel *k, struct jx_stats *st, char *why, unsigned whylen);

#endif /* JOIST_TEST_EXEC_H */
