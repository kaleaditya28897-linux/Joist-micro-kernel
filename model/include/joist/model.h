/* model.h - Joist host-side semantic model: public API (Phase 1).
 *
 * The model of the kernel's object model (plan §5.2, decisions 0001, 0003,
 * 0004). This header is the model's API, not yet the ABI: docs/abi.md is
 * written from it once Phase 1 settles it.
 *
 * Rules (model/README.md): no malloc, no recursion, no VLAs, fixed-width
 * types, bounded work per operation. All state lives in one caller-provided
 * struct joist_kernel with compile-time capacities; that struct stands in for
 * the memory donated at boot. Invariant numbers (I1...) refer to
 * docs/phase1-plan.md. */
#ifndef JOIST_MODEL_H
#define JOIST_MODEL_H

#include <stdint.h>

/* Capacities. Compile-time, as the kernel's will be (0003: preallocated, no
 * growth path at runtime). */
#ifndef JOIST_MAX_OBJECTS
#define JOIST_MAX_OBJECTS 256u
#endif
#ifndef JOIST_MAX_CSPACES
#define JOIST_MAX_CSPACES 16u
#endif
#ifndef JOIST_CSPACE_SLOTS
#define JOIST_CSPACE_SLOTS 64u
#endif

typedef enum {
    JOIST_OK = 0,
    JOIST_E_RANGE,    /* slot or capability-space index out of range */
    JOIST_E_EMPTY,    /* source slot holds no capability */
    JOIST_E_OCCUPIED, /* destination slot already holds a capability */
    JOIST_E_RIGHTS,   /* the operation needs a right the capability lacks */
    JOIST_E_NOMEM,    /* quota or object table exhausted (I3: never blocks) */
    JOIST_E_BADGE,    /* badge already set, or badge value invalid */
    JOIST_E_TYPE,     /* wrong object type for the operation */
    JOIST_E_DEAD      /* the capability space has been destroyed */
} joist_err;

typedef enum { JOIST_OBJ_NONE = 0, JOIST_OBJ_ENDPOINT, JOIST_OBJ_NOTIFICATION } joist_obj_type;

/* Rights bits (0001). */
typedef uint8_t joist_rights;
#define JOIST_R_SEND      ((joist_rights)0x1u)
#define JOIST_R_RECV      ((joist_rights)0x2u)
#define JOIST_R_SEND_ONCE ((joist_rights)0x4u)
#define JOIST_R_GRANT     ((joist_rights)0x8u)
#define JOIST_R_ALL       ((joist_rights)0xfu)

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
};

struct joist_cspace_state {
    uint8_t live;
    uint32_t quota;   /* objects this space may still create */
    uint32_t charged; /* live objects this space paid for (I3) */
    struct joist_cap slot[JOIST_CSPACE_SLOTS];
};

struct joist_kernel {
    struct joist_object obj[JOIST_MAX_OBJECTS];
    struct joist_cspace_state cs[JOIST_MAX_CSPACES];
    uint32_t objects_live;
};

/* Lifecycle. */
void joist_init(struct joist_kernel *k);

/* Create a capability space with an object quota. A test fixture until task
 * creation (Phase 1 work item 4) replaces it. */
joist_err joist_cspace_create(struct joist_kernel *k, uint32_t quota, joist_cspace *out);

/* Delete every capability in the space, then mark it dead (I1 end to end).
 * Bounded by JOIST_CSPACE_SLOTS. */
joist_err joist_cspace_destroy(struct joist_kernel *k, joist_cspace cs);

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

/* Read a capability (for tests and the eventual ABI's inspection call). */
joist_err joist_cap_read(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                         struct joist_cap *out);

/* Check the structural invariants over the whole model state. Returns the
 * number of violations; writes a description of the first into msg.
 * O(objects + spaces * slots): a test aid, not a kernel operation. */
unsigned joist_check_invariants(const struct joist_kernel *k, char *msg, unsigned msglen);

const char *joist_err_name(joist_err e);

#endif /* JOIST_MODEL_H */
