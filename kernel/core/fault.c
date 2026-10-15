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
 * except terminating a task, which is task destruction. */
#include "internal.h"

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

joist_err joist_thread_set_fault(struct joist_kernel *k, joist_cspace cs, joist_slot th_slot,
                                 joist_slot handler)
{
    uint32_t t;
    joist_err e = thread_of(k, cs, th_slot, &t);
    if (e != JOIST_OK) {
        return e;
    }
    if (handler != JOIST_SLOT_NONE && handler >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    k->th[t].fault_slot = handler == JOIST_SLOT_NONE ? 0 : handler + 1u;
    return JOIST_OK;
}

joist_err joist_task_set_fault(struct joist_kernel *k, joist_cspace cs, joist_slot task_slot,
                               joist_slot handler)
{
    uint32_t obj;
    joist_err e = joist__control_cap(k, cs, task_slot, JOIST_OBJ_TASK, &obj);
    if (e != JOIST_OK) {
        return e;
    }
    if (handler != JOIST_SLOT_NONE && handler >= JOIST_CSPACE_SLOTS) {
        return JOIST_E_RANGE;
    }
    k->cs[k->obj[obj].target - 1u].fault_slot = handler == JOIST_SLOT_NONE ? 0 : handler + 1u;
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

joist_err joist_fault(struct joist_kernel *k, joist_thread t, uint64_t kind, uint64_t addr,
                      uint64_t code)
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
    if (handler_ep(k, cs, r->fault_slot, &obj, &badge) ||
        handler_ep(k, cs, k->cs[cs].fault_slot, &obj, &badge)) {
        struct joist_msg m = {0};
        m.r[0] = kind;
        m.r[1] = addr;
        m.r[2] = code;
        return joist__fault_call(k, t, obj, badge, &m);
    }
    /* No handler: a faulting system with no handler must stop, not spin
     * (0009). The task goes; the root task can't (0013), so its thread does. */
    if (k->root == cs + 1u) {
        (void)joist_thread_destroy(k, t);
    } else {
        joist__space_destroy(k, cs);
        (void)joist__drain_pending(k);
    }
    return JOIST_E_PEER;
}
