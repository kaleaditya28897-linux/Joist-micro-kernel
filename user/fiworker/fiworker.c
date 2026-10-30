/* SPDX-License-Identifier: GPL-3.0-only */
/* fiworker.c - the fault-injection test's peer (Phase 3's exit; faultinj
 * loads it as every client and server): each thread carries out the
 * commands its root posts in their shared page (lib/fi.h), one at a time,
 * and writes back what each call returned. What it holds, and in which
 * slots, is the root's doing; the peer judges nothing, the root checks. */
#include "../lib/fi.h"
#include "../lib/joist.h"

void _start(uint64_t index);

static void spin(uint64_t n)
{
    for (volatile uint64_t i = 0; i < n; i++) {
    }
}

/* Wait for the root's next command. A thread holding a reply capability
 * may not Wait (E_STATE, docs/abi.md), so it watches the page instead. */
static void next(struct fi_thread *t, uint64_t go)
{
    while (t->posted == t->done) {
        uint64_t bits;
        if (j_wait(go, J_FOREVER, &bits) == J_E_STATE) {
            while (t->posted == t->done) {
            }
        }
    }
}

static uint64_t recv_into(struct fi_thread *t, uint64_t e, const struct j_msg *m)
{
    if (e == J_OK) {
        t->val[0] = m->badge;
        t->val[1] = m->r[0];
        t->val[2] = m->caps;
    }
    return e;
}

static uint64_t run(struct fi_thread *t)
{
    struct j_msg m = {0};
    uint64_t e;
    uint64_t v;
    switch (t->cmd) {
    case FI_CALL:
        m.r[0] = t->a[1];
        m.caps = t->a[2] == FI_NOCAP ? 0 : J_CAPS1(t->a[2], J_R_SEND);
        e = j_call(t->a[0], &m, t->a[3]);
        t->val[0] = m.r[0];
        return e;
    case FI_RECV:
        return recv_into(t, j_recv(t->a[0], &m, t->a[1]), &m);
    case FI_FIRST:
        e = j_recv(t->a[0], &m, J_FOREVER);
        if (e != J_OK) {
            return e;
        }
        m.caps = 0;
        return recv_into(t, j_reply_recv(t->a[0], &m, J_FOREVER), &m);
    case FI_REPLY_CMD:
        m.r[0] = t->a[0];
        return j_reply(&m);
    case FI_SAVE:
        return j_reply_save(t->a[0]);
    case FI_REPLY_SAVED:
        m.r[0] = t->a[1];
        return j_reply_saved(t->a[0], &m);
    case FI_DELETE:
        return j_cap_delete(t->a[0]);
    case FI_NSNEXT:
        e = j_nosenders_next(t->a[0], &v);
        t->val[0] = v;
        return e;
    case FI_BIND_NS:
        return j_bind_nosenders(t->a[0], t->a[1]);
    case FI_RECV_SLOTS:
        return j_recv_slots(t->a[0], J_SLOT_NONE, 1);
    case FI_SIGNALS:
        for (uint64_t i = 0; i < t->a[1]; i++) {
            t->intent = t->a[0] + i + 1u;
            e = j_signal(FI_SIG + t->a[0] + i);
            if (e != J_OK) {
                return e;
            }
            t->sent = t->a[0] + i + 1u;
            spin(t->a[2]);
        }
        return J_OK;
    case FI_WAITS:
        for (;;) {
            e = j_wait(t->a[0], J_FOREVER, &v);
            if (e != J_OK) {
                return e;
            }
            t->wakes = t->wakes + 1u;
            t->dups = t->dups + ((t->got & v & ~FI_STOP) != 0 ? 1u : 0u);
            t->got = t->got | (v & ~FI_STOP);
            if (v & FI_STOP) {
                return J_OK;
            }
        }
    case FI_EXIT:
        j_exit(0); /* from a task that isn't the root: this thread ends */
    default:
        return J_E_RANGE;
    }
}

void _start(uint64_t index)
{
    struct fi_page *pg = (struct fi_page *)(uintptr_t)FI_VA;
    struct fi_thread *t = &pg->t[index != 0 ? 1 : 0];
    uint64_t go = index != 0 ? FI_GO1 : FI_GO0;
    for (;;) {
        next(t, go);
        uint64_t n = t->posted;
        t->started = n;
        t->res = run(t);
        t->done = n;
    }
}
