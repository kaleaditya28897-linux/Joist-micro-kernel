/* SPDX-License-Identifier: GPL-3.0-only */
/* fault.c - thread control and fault handling (Phase 3 step 3; decisions
 * 0009, 0010).
 *
 * A thread control capability (CONTROL on a thread) reads and writes the
 * thread's register record, aborts its blocking operation, and suspends and
 * resumes it: holding the capability is the authority (0010). A fault is a
 * Call to the thread's fault handler, or its task's, found through a slot
 * in the faulting thread's own space when the fault happens; the call is
 * charged to the faulting thread's context like any Call (0009). The
 * handler's reply resumes or terminates the thread (ipc.c, deliver_reply);
 * a fault call that ends any other way leaves it suspended (ipc.c,
 * joist__wake), so nothing is destroyed from inside a wake-up. All O(1),
 * except terminating a task, which is task destruction.
 *
 * Two roles a task (0027): a page fault goes to the task's pager first, if
 * it has one; the pager may pass it on (JOIST_FAULT_PASS), and the kernel
 * then calls the exception handler with the same fault and the pager's
 * code (joist__fault_pass, from ipc.c's deliver_reply). At most one pass,
 * so a fault is at most two calls, one after the other: the thread is in
 * one fault call at a time, and its record says which (fault_route) and
 * keeps the fault's message for the second (fault_msg). */
#include "internal.h"

_Static_assert(JOIST_MSG_REGS >= 4u, "a fault message takes four registers");

/* The live thread named by a thread capability with CONTROL in cs. */
static joist_err thread_of(const struct joist_kernel *k, joist_cspace cs, joist_slot slot,
                           uint32_t *t)
{
    uint32_t obj;
    joist_err e = joist__control_cap(k, cs, slot, JOIST_OBJ_THREAD, &obj);
    if (e == JOIST_OK) {
        *t = k->obj[obj].target - 1u;
    }
    return e;
}

joist_err joist_thread_read_regs(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot,
                                 struct joist_regs *out)
{
    uint32_t t;
    joist_err e = thread_of(k, cs, th_slot, &t);
    if (e == JOIST_OK) {
        joist__off_cpu(k, t); /* while it runs, its record is behind it */
        *out = k->th[t].regs;
    }
    return e;
}

joist_err joist_thread_write_regs(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot,
                                  const struct joist_regs *in)
{
    uint32_t t;
    joist_err e = thread_of(k, cs, th_slot, &t);
    if (e == JOIST_OK) {
        joist__off_cpu(k, t); /* it would go on with the old ones, and save them over these */
        k->th[t].regs = *in;
    }
    return e;
}

static int blocked(const struct joist_thread_rec *r)
{
    return r->state == JOIST_T_BLOCKED_SEND || r->state == JOIST_T_BLOCKED_RECV ||
           r->state == JOIST_T_BLOCKED_REPLY || r->state == JOIST_T_BLOCKED_WAIT;
}

joist_err joist_thread_abort(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot)
{
    uint32_t t;
    joist_err e = thread_of(k, cs, th_slot, &t);
    if (e == JOIST_OK && blocked(&k->th[t])) {
        joist__cancel(k, t, JOIST_E_ABORTED);
    }
    return e;
}

joist_err joist_thread_suspend(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot)
{
    uint32_t t;
    joist_err e = thread_of(k, cs, th_slot, &t);
    if (e != JOIST_OK) {
        return e;
    }
    if (blocked(&k->th[t])) {
        joist__cancel(k, t, JOIST_E_ABORTED);
    }
    joist__off_cpu(k, t); /* stopped means not on a CPU either */
    k->th[t].state = JOIST_T_SUSPENDED;
    joist__rq_sync(k, t); /* stopped: off the queues */
    return JOIST_OK;
}

joist_err joist_thread_resume(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot)
{
    uint32_t t;
    joist_err e = thread_of(k, cs, th_slot, &t);
    if (e != JOIST_OK) {
        return e;
    }
    if (k->th[t].state != JOIST_T_SUSPENDED) {
        return JOIST_E_STATE;
    }
    k->th[t].state = JOIST_T_RUNNING;
    k->th[t].ready_seq = ++k->ready_seq; /* ready again: behind its equals */
    k->resched = 1;                      /* it can run again */
    joist__rq_sync(k, t);                /* on its queue */
    return JOIST_OK;
}

static int bad_handler(joist_slot handler, unsigned flags)
{
    return (handler != JOIST_SLOT_NONE && handler >= JOIST_CSPACE_SLOTS) ||
           (flags & ~(unsigned)JOIST_FAULT_NO_THREAD_CAP) != 0;
}

joist_err joist_thread_set_fault(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot,
                                 joist_slot handler, unsigned flags)
{
    uint32_t t;
    joist_err e = thread_of(k, cs, th_slot, &t);
    if (e != JOIST_OK) {
        return e;
    }
    if (bad_handler(handler, flags)) {
        return JOIST_E_RANGE;
    }
    k->th[t].fault_slot = handler == JOIST_SLOT_NONE ? 0 : handler + 1u;
    k->th[t].fault_flags = (uint8_t)flags;
    return JOIST_OK;
}

joist_err joist_task_set_fault(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                               joist_slot handler, unsigned flags)
{
    uint32_t obj;
    joist_err e = joist__control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &obj);
    if (e != JOIST_OK) {
        return e;
    }
    if (bad_handler(handler, flags)) {
        return JOIST_E_RANGE;
    }
    struct joist_cspace_state *st = &k->cs[k->obj[obj].target - 1u];
    st->fault_slot = handler == JOIST_SLOT_NONE ? 0 : handler + 1u;
    st->fault_flags = (uint8_t)flags;
    return JOIST_OK;
}

joist_err joist_task_set_pager(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                               joist_slot pager, unsigned flags)
{
    uint32_t obj;
    joist_err e = joist__control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &obj);
    if (e != JOIST_OK) {
        return e;
    }
    if (bad_handler(pager, flags)) {
        return JOIST_E_RANGE;
    }
    struct joist_cspace_state *st = &k->cs[k->obj[obj].target - 1u];
    st->pager_slot = pager == JOIST_SLOT_NONE ? 0 : pager + 1u;
    st->pager_flags = (uint8_t)flags;
    return JOIST_OK;
}

/* The endpoint a fault-handler slot (plus one) of space cs names, if it is
 * usable now: an endpoint capability with SEND whose endpoint is alive. */
static int handler_ep(const struct joist_kernel *k, uint32_t cs, uint32_t slot1, uint32_t *obj,
                      joist_badge *badge)
{
    if (slot1 == 0) {
        return 0;
    }
    const struct joist_cap *c = joist__slot_c(k, cs, slot1 - 1u);
    if (c == NULL || c->obj == 0 || c->type != JOIST_OBJ_ENDPOINT ||
        (c->rights & JOIST_R_SEND) == 0 || k->obj[c->obj - 1u].dead) {
        return 0;
    }
    *obj = c->obj - 1u;
    *badge = c->badge;
    return 1;
}

/* Thread t's exception handler (0009): its own, else its task's. */
static int exception_handler(const struct joist_kernel *k, uint32_t t, uint32_t *obj,
                             joist_badge *badge, unsigned *flags)
{
    const struct joist_thread_rec *r = &k->th[t];
    if (handler_ep(k, r->cs, r->fault_slot, obj, badge)) {
        *flags = r->fault_flags;
        return 1;
    }
    if (handler_ep(k, r->cs, k->cs[r->cs].fault_slot, obj, badge)) {
        *flags = k->cs[r->cs].fault_flags;
        return 1;
    }
    return 0;
}

/* The fault call of running thread t, its message kept in fault_msg, to
 * the endpoint found for this route. */
static joist_err fault_send(struct joist_kernel *k, uint32_t t, uint32_t obj, joist_badge badge,
                            unsigned flags, uint8_t route)
{
    struct joist_thread_rec *r = &k->th[t];
    struct joist_msg m = {0};
    m.r[0] = r->fault_msg[0];
    m.r[1] = r->fault_msg[1];
    m.r[2] = r->fault_msg[2];
    m.r[3] = r->fault_msg[3];
    uint64_t kind = JOIST_FAULT_KIND(m.r[0]);
    int with_cap = kind != JOIST_FAULT_PAGE || (flags & JOIST_FAULT_NO_THREAD_CAP) == 0;
    r->fault_route = route;
    return joist__fault_call(k, t, obj, badge, with_cap, &m);
}

/* No handler: a faulting system with no handler must stop, not spin
 * (0009). The task goes; the root task can't (0013), so its thread does. */
static joist_err no_handler(struct joist_kernel *k, uint32_t t)
{
    uint32_t cs = k->th[t].cs;
    if (k->root == cs + 1u) {
        (void)joist_thread_destroy(k, t);
    } else {
        joist__space_destroy(k, cs);
        (void)joist__drain_pending(k);
    }
    return JOIST_E_PEER;
}

joist_err joist_fault(struct joist_kernel *k, joist_thread t, uint64_t kind, uint64_t addr,
                      uint64_t code, uint64_t access)
{
    if (t >= JOIST_MAX_THREADS) {
        return JOIST_E_RANGE;
    }
    struct joist_thread_rec *r = &k->th[t];
    if (r->state != JOIST_T_RUNNING) {
        return JOIST_E_STATE;
    }
    uint32_t cs = r->cs;
    uint32_t obj;
    joist_badge badge;
    unsigned flags = 0;
    struct joist_msg m = {0};
    m.r[0] = kind | (kind == JOIST_FAULT_PAGE ? (access & 0xffu) << 8 : 0u);
    m.r[1] = addr;
    m.r[2] = code;
    m.r[3] = r->regs.pc;
    r->fault_msg[0] = m.r[0]; /* kept for a pass (0027) */
    r->fault_msg[1] = m.r[1];
    r->fault_msg[2] = m.r[2];
    r->fault_msg[3] = m.r[3];
    /* A page fault: the task's pager, if it has one usable (0027). */
    if (kind == JOIST_FAULT_PAGE && handler_ep(k, cs, k->cs[cs].pager_slot, &obj, &badge)) {
        return fault_send(k, t, obj, badge, k->cs[cs].pager_flags, JOIST_ROUTE_PAGER);
    }
    if (exception_handler(k, t, &obj, &badge, &flags)) {
        return fault_send(k, t, obj, badge, flags, JOIST_ROUTE_HANDLER);
    }
    return no_handler(k, t);
}

void joist__fault_pass(struct joist_kernel *k, uint32_t t, uint64_t code)
{
    /* The same fault, marked passed, with the pager's code beside the kind
     * and access (r[0]'s upper half: no register is free). */
    struct joist_thread_rec *r = &k->th[t];
    r->fault_msg[0] = (r->fault_msg[0] & 0xffffu) | JOIST_FAULT_PASSED | (code & 0xffffffffu) << 32;
    uint32_t obj;
    joist_badge badge;
    unsigned flags = 0;
    if (exception_handler(k, t, &obj, &badge, &flags)) {
        (void)fault_send(k, t, obj, badge, flags, JOIST_ROUTE_PASSED);
        return;
    }
    (void)no_handler(k, t);
}
