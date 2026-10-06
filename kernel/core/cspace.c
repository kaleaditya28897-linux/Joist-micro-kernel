/* cspace.c - capability spaces, objects and capability derivation (Phase 1
 * work item 1; invariants I1, I3, I7, I8).
 *
 * Every operation here touches a fixed number of slots and objects, except
 * joist_cspace_destroy, which is bounded by JOIST_CSPACE_SLOTS (I9). */
#include "internal.h"

/* Full rights an object of each type is created with. */
static joist_rights full_rights(joist_obj_type t)
{
    switch (t) {
    case JOIST_OBJ_ENDPOINT:
    case JOIST_OBJ_NOTIFICATION:
        return (joist_rights)(JOIST_R_SEND | JOIST_R_RECV | JOIST_R_GRANT);
    case JOIST_OBJ_TASK:
    case JOIST_OBJ_THREAD:
    case JOIST_OBJ_REPLY:
        /* CONTROL on tasks and threads; on a reply object, SEND: may reply.
         * The same bit (JOIST_R_CONTROL is JOIST_R_SEND). */
        return (joist_rights)(JOIST_R_SEND | JOIST_R_GRANT);
    case JOIST_OBJ_NONE:
    default:
        return 0;
    }
}

void joist_init(struct joist_kernel *k)
{
    memset(k, 0, sizeof *k);
}

static joist_err get_space(struct joist_kernel *k, joist_cspace cs, struct joist_cspace_state **out)
{
    if (cs >= JOIST_MAX_CSPACES) {
        return JOIST_E_RANGE;
    }
    if (!k->cs[cs].live) {
        return JOIST_E_DEAD;
    }
    *out = &k->cs[cs];
    return JOIST_OK;
}

joist_err joist_cspace_create(struct joist_kernel *k, uint32_t quota, joist_cspace *out)
{
    for (uint32_t i = 0; i < JOIST_MAX_CSPACES; i++) {
        JOIST_WORK(k);
        struct joist_cspace_state *s = &k->cs[i];
        /* A destroyed space is not reused while it is still charged for
         * objects that outlive it, so the charge stays attributable. */
        if (!s->live && s->charged == 0) {
            memset(s, 0, sizeof *s);
            s->live = 1;
            s->quota = quota;
            s->fixture = 1;
            k->budget += quota; /* fixture quota enters from nothing (I3) */
            *out = i;
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

/* Drop one reference; destroy the object and refund its donor on the last. */
static void obj_unref(struct joist_kernel *k, uint32_t obj_index)
{
    struct joist_object *o = &k->obj[obj_index];
    o->refs--;
    if (o->refs == 0) {
        joist__object_freed(k, obj_index);    /* waiters wake; kernel references dropped */
        joist__lifecycle_freed(k, obj_index); /* an unreachable task or thread goes too */
        struct joist_cspace_state *donor = &k->cs[o->donor];
        donor->charged--;
        if (donor->live) {
            donor->quota++;
        } else {
            k->budget--; /* a dead parentless donor: the unit leaves (I3) */
        }
        o->type = JOIST_OBJ_NONE;
        o->donor = 0;
        o->target = 0;
        o->rseq = 0;
        o->qkind = JOIST_Q_NONE;
        o->qhead = 0;
        o->qtail = 0;
        o->recv_caps = 0;
        o->dead = 0;
        o->ns_notif = 0;
        o->ns_bits = 0;
        o->ns_first = 0;
        o->ns_last = 0;
        o->word = 0;
        k->objects_live--;
    }
}

static joist_err slot_check(joist_slot slot)
{
    return slot < JOIST_CSPACE_SLOTS ? JOIST_OK : JOIST_E_RANGE;
}

joist_err joist_obj_create(struct joist_kernel *k, joist_cspace cs, joist_obj_type type,
                           joist_slot dst)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    if (slot_check(dst) != JOIST_OK) {
        return JOIST_E_RANGE;
    }
    /* Tasks and threads have their own constructors (task.c). */
    if (full_rights(type) == 0 || type == JOIST_OBJ_TASK || type == JOIST_OBJ_THREAD) {
        return JOIST_E_TYPE;
    }
    if (s->slot[dst].obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    if (s->quota == 0) {
        return JOIST_E_NOMEM;
    }
    /* Linear scan of a fixed-size table: bounded by JOIST_MAX_OBJECTS. The
     * kernel will keep a free list; the bound, not the constant, is the point. */
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        JOIST_WORK(k);
        struct joist_object *o = &k->obj[i];
        if (o->type == JOIST_OBJ_NONE) {
            /* Start from zero, not from whatever freeing left (CBMC: a reused
             * slot must not carry a reply binding or a queue). */
            memset(o, 0, sizeof *o);
            o->type = (uint8_t)type;
            o->refs = 1;
            o->donor = cs;
            s->quota--;
            s->charged++;
            k->objects_live++;
            s->slot[dst] = (struct joist_cap){
                .obj = i + 1u, .type = (uint8_t)type, .rights = full_rights(type), .badge = 0};
            joist__cap_added(k, &s->slot[dst]);
            return JOIST_OK;
        }
    }
    return JOIST_E_NOMEM;
}

/* Shared body of derive and mint. */
static joist_err copy_cap(struct joist_kernel *k, joist_cspace cs, joist_slot src, joist_slot dst,
                          joist_rights mask, int set_badge, joist_badge badge)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    if (slot_check(src) != JOIST_OK || slot_check(dst) != JOIST_OK) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *from = &s->slot[src];
    if (from->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if (s->slot[dst].obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    if (set_badge) {
        if ((from->rights & JOIST_R_GRANT) == 0) {
            return JOIST_E_RIGHTS;
        }
        if (from->badge != 0 || badge == 0) {
            return JOIST_E_BADGE;
        }
    }
    struct joist_cap to = *from;
    to.rights = (joist_rights)(from->rights & mask); /* I8: a subset, never more */
    if (set_badge) {
        to.badge = badge;
        /* The first send capability for a new badge needs its no-senders
         * account (I4). Allocated here and only here, before anything is
         * changed, so a full table fails the mint cleanly. */
        if (to.type == JOIST_OBJ_ENDPOINT && (to.rights & JOIST_R_SEND) != 0) {
            joist_err r = joist__acct_reserve(k, from->obj - 1u, badge);
            if (r != JOIST_OK) {
                return r;
            }
        }
    }
    k->obj[from->obj - 1u].refs++;
    s->slot[dst] = to;
    joist__cap_added(k, &s->slot[dst]);
    return JOIST_OK;
}

joist_err joist_cap_derive(struct joist_kernel *k, joist_cspace cs, joist_slot src, joist_slot dst,
                           joist_rights mask)
{
    return copy_cap(k, cs, src, dst, mask, 0, 0);
}

joist_err joist_cap_mint(struct joist_kernel *k, joist_cspace cs, joist_slot src, joist_slot dst,
                         joist_rights mask, joist_badge badge)
{
    return copy_cap(k, cs, src, dst, mask, 1, badge);
}

joist_err joist__cap_grant(struct joist_kernel *k, joist_cspace from, joist_slot src,
                           joist_cspace to, joist_slot dst, joist_rights mask)
{
    struct joist_cspace_state *fs;
    struct joist_cspace_state *ts;
    joist_err e = get_space(k, from, &fs);
    if (e == JOIST_OK) {
        e = get_space(k, to, &ts);
    }
    if (e != JOIST_OK) {
        return e;
    }
    if (slot_check(src) != JOIST_OK || slot_check(dst) != JOIST_OK) {
        return JOIST_E_RANGE;
    }
    const struct joist_cap *c = &fs->slot[src];
    if (c->obj == 0) {
        return JOIST_E_EMPTY;
    }
    if ((c->rights & JOIST_R_GRANT) == 0) {
        return JOIST_E_RIGHTS;
    }
    if (ts->slot[dst].obj != 0) {
        return JOIST_E_OCCUPIED;
    }
    /* Same object and badge, so any no-senders account already exists. */
    struct joist_cap copy = *c;
    copy.rights = (joist_rights)(c->rights & mask); /* I8 */
    k->obj[c->obj - 1u].refs++;
    ts->slot[dst] = copy;
    joist__cap_added(k, &ts->slot[dst]);
    return JOIST_OK;
}

void joist__cap_delete_slot(struct joist_kernel *k, uint32_t s, joist_slot slot)
{
    struct joist_cap *c = &k->cs[s].slot[slot];
    struct joist_cap gone = *c;
    memset(c, 0, sizeof *c);
    joist__cap_removed(k, &gone); /* may fire no-senders or kill the endpoint */
    obj_unref(k, gone.obj - 1u);  /* may queue an unreachable task */
}

joist_err joist_cap_delete(struct joist_kernel *k, joist_cspace cs, joist_slot slot)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    if (slot_check(slot) != JOIST_OK) {
        return JOIST_E_RANGE;
    }
    if (s->slot[slot].obj == 0) {
        return JOIST_E_EMPTY;
    }
    joist__cap_delete_slot(k, cs, slot);
    joist__drain_pending(k);
    return JOIST_OK;
}

joist_err joist_cspace_destroy(struct joist_kernel *k, joist_cspace cs)
{
    struct joist_cspace_state *s;
    joist_err e = get_space(k, cs, &s);
    if (e != JOIST_OK) {
        return e;
    }
    if (k->root == cs + 1u) {
        return JOIST_E_STATE; /* the root task never dies (0013) */
    }
    joist__space_destroy(k, cs);
    joist__drain_pending(k);
    return JOIST_OK;
}

joist_err joist_cap_read(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                         struct joist_cap *out)
{
    if (cs >= JOIST_MAX_CSPACES || slot_check(slot) != JOIST_OK) {
        return JOIST_E_RANGE;
    }
    if (!k->cs[cs].live) {
        return JOIST_E_DEAD;
    }
    *out = k->cs[cs].slot[slot];
    return out->obj != 0 ? JOIST_OK : JOIST_E_EMPTY;
}

unsigned joist_check_invariants(const struct joist_kernel *k, char *msg, unsigned msglen)
{
    unsigned bad = 0;
    uint32_t refs_seen[JOIST_MAX_OBJECTS];
    uint32_t charged_seen[JOIST_MAX_CSPACES];
    uint32_t live = 0;

    memset(refs_seen, 0, sizeof refs_seen);
    memset(charged_seen, 0, sizeof charged_seen);

#define VIOLATION(...)                                                                             \
    do {                                                                                           \
        if (bad++ == 0 && msg != NULL && msglen > 0) {                                             \
            JOIST_FMT(msg, msglen, __VA_ARGS__);                                              \
        }                                                                                          \
    } while (0)

    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        const struct joist_cspace_state *s = &k->cs[c];
        for (uint32_t i = 0; i < JOIST_CSPACE_SLOTS; i++) {
            const struct joist_cap *cap = &s->slot[i];
            if (cap->obj == 0) {
                continue;
            }
            if (!s->live) {
                VIOLATION("I1: dead space %u still holds a capability in slot %u", c, i);
                continue;
            }
            if (cap->obj > JOIST_MAX_OBJECTS) {
                VIOLATION("I1: space %u slot %u names object %u out of range", c, i, cap->obj);
                continue;
            }
            const struct joist_object *o = &k->obj[cap->obj - 1u];
            if (o->type == JOIST_OBJ_NONE) {
                VIOLATION("I1: space %u slot %u names freed object %u", c, i, cap->obj - 1u);
                continue;
            }
            if (cap->type != o->type) {
                VIOLATION("I1: space %u slot %u type %u != object type %u", c, i, cap->type,
                          o->type);
            }
            if ((cap->rights & (joist_rights)~full_rights((joist_obj_type)o->type)) != 0) {
                VIOLATION("I8: space %u slot %u has rights 0x%x beyond the type's 0x%x", c, i,
                          cap->rights, full_rights((joist_obj_type)o->type));
            }
            refs_seen[cap->obj - 1u]++;
        }
    }
    for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
        const struct joist_object *o = &k->obj[i];
        if (o->type == JOIST_OBJ_NONE) {
            if (o->refs != 0) {
                VIOLATION("I1: freed object %u has refs %u", i, o->refs);
            }
            continue;
        }
        live++;
        if (o->refs != refs_seen[i]) {
            VIOLATION("I1: object %u refs %u but %u capabilities name it", i, o->refs,
                      refs_seen[i]);
        }
        if (o->refs == 0) {
            VIOLATION("I1: live object %u has no capabilities (leaked)", i);
        }
        if (o->donor >= JOIST_MAX_CSPACES) {
            VIOLATION("I3: object %u donor %u out of range", i, o->donor);
        } else {
            charged_seen[o->donor]++;
        }
    }
    if (live != k->objects_live) {
        VIOLATION("I3: objects_live %u but %u objects are live", k->objects_live, live);
    }
    for (uint32_t c = 0; c < JOIST_MAX_CSPACES; c++) {
        if (k->cs[c].charged != charged_seen[c]) {
            VIOLATION("I3: space %u charged %u but donated %u live objects", c, k->cs[c].charged,
                      charged_seen[c]);
        }
    }
#undef VIOLATION
    unsigned ipc = joist__check_ipc(k, msg, msglen, bad == 0);
    unsigned note = joist__check_notify(k, msg, msglen, bad + ipc == 0);
    return bad + ipc + note + joist__check_task(k, msg, msglen, bad + ipc + note == 0);
}

#if !__STDC_HOSTED__
/* Freestanding JOIST_FMT: copy the format text itself, bounded. */
void joist__fmt_bare(char *msg, unsigned msglen, const char *fmt)
{
    if (msglen == 0) {
        return;
    }
    unsigned i = 0;
    for (; i + 1u < msglen && fmt[i] != '\0'; i++) {
        msg[i] = fmt[i];
    }
    msg[i] = '\0';
}
#endif

const char *joist_err_name(joist_err e)
{
    switch (e) {
    case JOIST_OK:
        return "OK";
    case JOIST_E_RANGE:
        return "E_RANGE";
    case JOIST_E_EMPTY:
        return "E_EMPTY";
    case JOIST_E_OCCUPIED:
        return "E_OCCUPIED";
    case JOIST_E_RIGHTS:
        return "E_RIGHTS";
    case JOIST_E_NOMEM:
        return "E_NOMEM";
    case JOIST_E_BADGE:
        return "E_BADGE";
    case JOIST_E_TYPE:
        return "E_TYPE";
    case JOIST_E_DEAD:
        return "E_DEAD";
    case JOIST_E_TIMEOUT:
        return "E_TIMEOUT";
    case JOIST_E_GONE:
        return "E_GONE";
    case JOIST_E_PEER:
        return "E_PEER";
    case JOIST_E_NOREPLY:
        return "E_NOREPLY";
    case JOIST_E_STATE:
        return "E_STATE";
    case JOIST_BLOCKED:
        return "BLOCKED";
    default:
        return "E_?";
    }
}
