/* SPDX-License-Identifier: GPL-3.0-only */
/* abi.c - the system-call ABI: registers to core operations. See abi.h. */
#include "abi.h"

#include "internal.h"
#include "usercopy.h"

/* A slot argument: anything past 32 bits is out of range, never truncated
 * into a valid slot. (JOIST_SLOT_SELF and JOIST_SLOT_NONE fit 32 bits and
 * pass through, for the calls that take them.) */
static joist_slot slot_arg(uint64_t v)
{
    return v > UINT32_MAX ? JOIST_CSPACE_SLOTS : (joist_slot)v;
}

static uint32_t u32_arg(uint64_t v)
{
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

uint64_t abi_caps_pack(const struct joist_msg *m)
{
    uint64_t v = m->ncaps <= JOIST_MSG_CAPS ? m->ncaps : JOIST_MSG_CAPS;
    for (uint32_t i = 0; i < JOIST_MSG_CAPS; i++) {
        uint64_t slot = m->cap[i] == JOIST_SLOT_NONE || m->cap[i] > 0xfffeu ? ABI_CAPS_NONE
                                                                            : m->cap[i];
        v |= slot << (2u + 20u * i);
        v |= (uint64_t)(m->mask[i] & 0xfu) << (18u + 20u * i);
    }
    return v;
}

void abi_caps_unpack(uint64_t v, struct joist_msg *m)
{
    /* A count of 3 is past JOIST_MSG_CAPS: kept, so the send check refuses
     * it (JOIST_E_RANGE), as the model does. */
    m->ncaps = (uint32_t)(v & 3u);
    for (uint32_t i = 0; i < JOIST_MSG_CAPS; i++) {
        uint32_t slot = (uint32_t)((v >> (2u + 20u * i)) & 0xffffu);
        m->cap[i] = slot == ABI_CAPS_NONE ? JOIST_CSPACE_SLOTS : slot;
        m->mask[i] = (joist_rights)((v >> (18u + 20u * i)) & 0xfu);
    }
}

static void msg_in(const struct joist_regs *r, struct joist_msg *m)
{
    *m = (struct joist_msg){0};
    for (uint32_t i = 0; i < JOIST_MSG_REGS; i++) {
        m->r[i] = r->r[ABI_R12 + i];
    }
    abi_caps_unpack(r->r[ABI_R9], m);
    m->len = (uint32_t)((r->r[ABI_R9] >> ABI_LEN_SHIFT) & ABI_LEN_MASK);
}

/* A finished receive's outputs, from the thread's record. */
static void outputs(struct joist_kernel *k, uint32_t t, joist_err e)
{
    struct joist_thread_rec *th = &k->th[t];
    th->regs.r[ABI_RAX] = (uint64_t)e;
    th->regs.r[ABI_RDX] = th->badge;
    for (uint32_t i = 0; i < JOIST_MSG_REGS; i++) {
        th->regs.r[ABI_R12 + i] = th->msg.r[i];
    }
    th->regs.r[ABI_R9] = abi_caps_pack(&th->msg) |
                         (uint64_t)(th->msg.len & ABI_LEN_MASK) << ABI_LEN_SHIFT |
                         (th->msg.lost ? ABI_LOST_BIT : 0);
}

void abi_complete(struct abi_env *env, uint32_t t)
{
    struct joist_thread_rec *th = &env->k->th[t];
    if (th->state == JOIST_T_FREE) {
        env->pending[t] = 0; /* the slot was freed; a new thread may come */
        return;
    }
    if (env->pending[t] && th->state == JOIST_T_RUNNING) {
        outputs(env->k, t, th->result);
        env->pending[t] = 0;
    }
}

int abi_syscall(struct abi_env *env, uint32_t t)
{
    struct joist_kernel *k = env->k;
    struct joist_regs *R = &k->th[t].regs;
    uint32_t cs = k->th[t].cs;
    uint64_t nr = R->r[ABI_RAX];
    uint64_t a0 = R->r[ABI_RDI];
    uint64_t a1 = R->r[ABI_RSI];
    uint64_t a2 = R->r[ABI_RDX];
    uint64_t a3 = R->r[ABI_R10];
    struct joist_msg m;
    joist_err e;
    uint64_t out = R->r[ABI_RDX]; /* RDX unless the call returns something */
    /* For the direct switch: the caller's place in line before the call,
     * and whom a ReplyRecv answers. */
    uint64_t place = k->th[t].ready_seq;
    uint32_t answered = k->th[t].reply_to;
    env->handover = JOIST_MAX_THREADS;

    switch (nr) {
    case ABI_CALL:
        msg_in(R, &m);
        e = joist_call(k, t, slot_arg(a0), &m, a1);
        break;
    case ABI_RECV:
        e = joist_recv(k, t, slot_arg(a0), a1);
        break;
    case ABI_REPLY:
        msg_in(R, &m);
        e = joist_reply(k, t, &m);
        break;
    case ABI_REPLY_RECV:
        msg_in(R, &m);
        e = joist_reply_recv(k, t, &m, slot_arg(a0), a1);
        break;
    case ABI_SIGNAL:
        e = joist_signal(k, t, slot_arg(a0));
        break;
    case ABI_WAIT:
        e = joist_wait(k, t, slot_arg(a0), a1);
        break;
    case ABI_RECV_SLOTS: {
        joist_slot s[2] = {slot_arg(a0), slot_arg(a1)};
        e = joist_recv_slots(k, t, s, u32_arg(a2));
        break;
    }
    case ABI_REPLY_SAVE:
        e = joist_reply_save(k, t, slot_arg(a0));
        break;
    case ABI_REPLY_SAVED:
        msg_in(R, &m);
        e = joist_reply_saved(k, t, slot_arg(a0), &m);
        break;
    case ABI_OBJ_CREATE:
        /* A type past the last one is no type (E_TYPE), never an enum value
         * out of range. */
        e = joist_obj_create(k, cs, a0 <= JOIST_OBJ_FRAME ? (joist_obj_type)a0 : JOIST_OBJ_NONE,
                             slot_arg(a1));
        break;
    case ABI_CAP_DERIVE:
        e = joist_cap_derive(k, cs, slot_arg(a0), slot_arg(a1), (joist_rights)(a2 & 0xffu));
        break;
    case ABI_CAP_MINT:
        e = joist_cap_mint(k, cs, slot_arg(a0), slot_arg(a1), (joist_rights)(a2 & 0xffu), a3);
        break;
    case ABI_CAP_DELETE:
        e = joist_cap_delete(k, cs, slot_arg(a0));
        break;
    case ABI_CAP_READ: {
        struct joist_cap c = {0};
        e = joist_cap_read(k, cs, slot_arg(a0), &c);
        out = (uint64_t)c.type | (uint64_t)c.rights << 8;
        R->r[ABI_R8] = c.badge;
        R->r[ABI_R10] = e == JOIST_OK && c.type == JOIST_OBJ_FRAME ? k->obj[c.obj - 1u].npages : 0;
        break;
    }
    case ABI_CAP_GIVE:
        e = joist_cap_give(k, cs, slot_arg(a0), slot_arg(a1), slot_arg(a2), (joist_rights)(a3 & 0xffu));
        break;
    case ABI_CAP_COPY_BATCH: {
        /* The fifth argument in R8; the bitmap of what was skipped comes
         * back there too, as cap_read's badge does. */
        uint32_t done = 0;
        uint32_t skipped = 0;
        e = joist_cap_copy_batch(k, cs, slot_arg(a0), slot_arg(a1), u32_arg(a2),
                                 (joist_rights)(a3 & 0xffu), u32_arg(R->r[ABI_R8]), &done,
                                 &skipped);
        out = done;
        R->r[ABI_R8] = skipped;
        break;
    }
    case ABI_OBJ_KILL:
        e = joist_obj_kill(k, cs, slot_arg(a0));
        break;
    case ABI_CSPACE_GROW:
        e = joist_cspace_grow(k, cs, u32_arg(a0));
        break;
    case ABI_CSPACE_SHRINK:
        e = joist_cspace_shrink(k, cs, u32_arg(a0));
        break;
    case ABI_TASK_CREATE:
        e = joist_task_create(k, cs, u32_arg(a0), slot_arg(a1));
        break;
    case ABI_TASK_DESTROY:
        e = joist_task_destroy(k, cs, slot_arg(a0));
        break;
    case ABI_THREAD_SPAWN: {
        joist_thread nt;
        e = joist_thread_spawn(k, cs, slot_arg(a0), slot_arg(a1), &nt);
        if (e == JOIST_OK) {
            /* It starts at pc with sp, everything else zero; the kernel
             * sanitises the flags on the way out. */
            k->th[nt].regs = (struct joist_regs){0};
            k->th[nt].regs.pc = a2;
            k->th[nt].regs.sp = a3;
            k->th[nt].regs.flags = 0x202u;
            env->exact[nt] = 1;
            env->fresh[nt] = 1;
            /* A thread killed while its call waited never came back for
             * abi_complete to clear this: its outputs would land in the
             * new thread's registers. */
            env->pending[nt] = 0;
        }
        break;
    }
    case ABI_THREAD_KILL:
        e = joist_thread_kill(k, cs, slot_arg(a0));
        break;
    case ABI_THREAD_SUSPEND:
        e = joist_thread_suspend(k, cs, slot_arg(a0));
        break;
    case ABI_THREAD_RESUME:
        e = joist_thread_resume(k, cs, slot_arg(a0));
        break;
    case ABI_THREAD_ABORT:
        e = joist_thread_abort(k, cs, slot_arg(a0));
        break;
    case ABI_THREAD_SET_FAULT:
        e = joist_thread_set_fault(k, cs, slot_arg(a0), slot_arg(a1), u32_arg(a2));
        break;
    case ABI_TASK_SET_FAULT:
        e = joist_task_set_fault(k, cs, slot_arg(a0), slot_arg(a1), u32_arg(a2));
        break;
    case ABI_FRAME_SPLIT:
        e = joist_frame_split(k, cs, slot_arg(a0), a1, slot_arg(a2));
        break;
    case ABI_MAP:
        e = joist_map(k, cs, slot_arg(a0), a1, a2, slot_arg(a3), R->r[ABI_R8], u32_arg(R->r[ABI_R9]));
        break;
    case ABI_MAP_REPLACE:
        e = joist_map_replace(k, cs, slot_arg(a0), a1, a2, slot_arg(a3), R->r[ABI_R8],
                              u32_arg(R->r[ABI_R9]), slot_arg(R->r[ABI_R12]), R->r[ABI_R13]);
        break;
    case ABI_UNMAP:
        e = joist_unmap_range(k, cs, slot_arg(a0), a1, a2);
        break;
    case ABI_PROTECT:
        e = joist_protect(k, cs, slot_arg(a0), a1, a2, u32_arg(a3));
        break;
    case ABI_SPACE_DONATE:
        e = joist_space_donate(k, cs, slot_arg(a0), slot_arg(a1));
        break;
    case ABI_MAP_QUERY: {
        /* As ABI_THREAD_READ_REGS: the answer first, then the caller's
         * memory; one that can't take it whole is E_RANGE, nothing written. */
        uint64_t q[JOIST_QUERY_MAX];
        e = joist_map_query(k, cs, slot_arg(a0), a1, a2, slot_arg(a3), q);
        if (e == JOIST_OK && copy_to_user(env->space_of(env->ctx, cs), R->r[ABI_R8], q,
                                          (size_t)a2 * sizeof q[0]) != KERR_OK) {
            e = JOIST_E_RANGE;
        }
        break;
    }
    case ABI_REGION_CREATE:
        e = joist_region_create(k, cs, slot_arg(a0), a1, u32_arg(a2), u32_arg(a3),
                                slot_arg(R->r[ABI_R8]));
        break;
    case ABI_REGION_ADD: {
        uint32_t x = 0;
        e = joist_region_add(k, cs, slot_arg(a0), slot_arg(a1), a2, a3, &x);
        if (e == JOIST_OK) {
            out = x;
        }
        break;
    }
    case ABI_REGION_MAP:
        e = joist_region_map(k, cs, slot_arg(a0), u32_arg(a1), a2, a3, R->r[ABI_R8],
                             u32_arg(R->r[ABI_R9]));
        break;
    case ABI_REGION_UNMAP:
        e = joist_region_unmap(k, cs, slot_arg(a0), a1, a2);
        break;
    case ABI_REGION_PROTECT:
        e = joist_region_protect(k, cs, slot_arg(a0), a1, a2, u32_arg(a3));
        break;
    case ABI_SC_CREATE:
        e = a1 > JOIST_PRIO_MAX ? JOIST_E_RIGHTS
                                : joist_sc_create(k, cs, slot_arg(a0), (uint8_t)a1, a2, a3);
        break;
    case ABI_SC_BIND:
        e = joist_sc_bind(k, cs, slot_arg(a0), slot_arg(a1));
        break;
    case ABI_SC_UNBIND:
        e = joist_sc_unbind(k, cs, slot_arg(a0));
        break;
    case ABI_SC_BIND_FALLBACK:
        e = joist_sc_bind_fallback(k, cs, slot_arg(a0), slot_arg(a1));
        break;
    case ABI_TASK_SET_MCP:
        e = a1 > JOIST_PRIO_MAX ? JOIST_E_RIGHTS : joist_task_set_mcp(k, cs, slot_arg(a0), (uint8_t)a1);
        break;
    case ABI_YIELD:
        e = joist_yield(k, t);
        break;
    case ABI_CLOCK:
        /* The core's clock, as read at this entry (monotonic), and the
         * CPUs: what glibc's clock_gettime and CPU count stand on. */
        e = JOIST_OK;
        out = k->now;
        R->r[ABI_R10] = env->ncpus != 0 ? env->ncpus : 1u;
        break;
    case ABI_CONTINUE:
        /* No caller may ask for more at once than the kernel's own
         * preemption limit allows (0 from a caller would mean all of it). */
        e = joist_continue(k, k->preempt != 0 && (a0 == 0 || a0 > k->preempt) ? k->preempt
                                                                              : u32_arg(a0));
        if (e == JOIST_BLOCKED) {
            out = 1; /* more remains: not a wait, just an answer */
            e = JOIST_OK;
        } else {
            out = 0;
        }
        break;
    case ABI_TASK_REAP:
        e = joist_task_reap(k, cs, u32_arg(a0));
        break;
    case ABI_TASK_DESCENDANT: {
        joist_cspace d = 0;
        e = joist_task_descendant(k, cs, u32_arg(a0), &d);
        out = d;
        break;
    }
    case ABI_NOSENDERS_NEXT: {
        joist_badge b = 0;
        e = joist_nosenders_next(k, cs, slot_arg(a0), &b);
        out = b;
        break;
    }
    case ABI_BIND_NOSENDERS:
        e = joist_bind_nosenders(k, cs, slot_arg(a0), slot_arg(a1));
        break;
    case ABI_REQUEST_DEADNAME:
        e = joist_request_deadname(k, cs, slot_arg(a0), slot_arg(a1));
        break;
    case ABI_DEBUG_COUNTERS: {
        /* For the root task alone: it says how much of the machine is in
         * use by everyone. joist_err results, unlike calls 1 and 2. */
        struct abi_counters c = {0};
        e = JOIST_OK;
        if (k->root != cs + 1u) {
            e = JOIST_E_RIGHTS;
            break;
        }
        c.objects = k->objects_live;
        for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
            c.threads += k->th[i].state != JOIST_T_FREE;
        }
        for (uint32_t i = 0; i < JOIST_MAX_CSPACES; i++) {
            c.spaces += k->cs[i].live != 0;
        }
        for (uint32_t i = 0; i < JOIST_MAX_LEAVES; i++) {
            c.leaves += k->leaf[i].owner != 0;
        }
        for (uint32_t i = 0; i < JOIST_MAX_OBJECTS; i++) {
            c.mappings += k->obj[i].type == JOIST_OBJ_FRAME ? k->obj[i].mapcount : 0;
        }
        for (uint32_t i = 0; i < JOIST_MAX_CSPACES; i++) {
            c.table_pages += k->cs[i].pt_used;
        }
        c.pending = k->npending;
        c.rq_walks = k->rq_walks;
        if (env->machine_counters != NULL) {
            env->machine_counters(env->ctx, &c);
        }
        if (copy_to_user(env->space_of(env->ctx, cs), a0, &c, sizeof c) != KERR_OK) {
            e = JOIST_E_RANGE;
        }
        break;
    }
    case ABI_IPC_BUFFER:
        e = joist_ipc_buffer(k, t, a0);
        break;
    case ABI_THREAD_READ_REGS: {
        /* The capability first, then the caller's memory: a record that
         * can't be written whole is E_RANGE and nothing is written. */
        struct joist_regs regs;
        e = joist_thread_read_regs(k, cs, slot_arg(a0), &regs);
        if (e == JOIST_OK &&
            copy_to_user(env->space_of(env->ctx, cs), a1, &regs, sizeof regs) != KERR_OK) {
            e = JOIST_E_RANGE;
        }
        break;
    }
    case ABI_THREAD_WRITE_REGS: {
        /* Any values at all: the way out to user mode trusts none of them
         * (flags, the TLS base and the pc are checked there, for every
         * thread, every time). A thread blocked in a call still gets that
         * call's outputs when it wakes, over what is written here. */
        struct joist_regs regs;
        e = joist_thread_read_regs(k, cs, slot_arg(a0), &regs); /* the capability's checks */
        if (e == JOIST_OK) {
            e = copy_from_user(env->space_of(env->ctx, cs), &regs, a1, sizeof regs) == KERR_OK
                    ? joist_thread_write_regs(k, cs, slot_arg(a0), &regs)
                    : JOIST_E_RANGE;
        }
        if (e == JOIST_OK) {
            /* All of them count from here, RCX and R11 too. */
            env->exact[k->obj[joist__slot_c(k, cs, slot_arg(a0))->obj - 1u].target - 1u] = 1;
        }
        break;
    }
    default: {
        /* Phase 2's calls, and every unknown number, through its dispatcher. */
        struct sys_env se = {.task = env->space_of(env->ctx, cs), .out = env->out, .ctx = env->ctx};
        uint64_t r = sys_dispatch(&se, nr, a0, a1);
        if (se.exited) {
            if (k->root == cs + 1u) {
                env->finished = 1; /* the root task is done: so is the run */
                env->exit_code = se.exit_code;
            } else {
                (void)joist_thread_destroy(k, t); /* a task's thread ends */
            }
            return ABI_GONE;
        }
        R->r[ABI_RAX] = r;
        return ABI_DONE;
    }
    }

    if (k->th[t].state == JOIST_T_FREE) {
        return ABI_GONE; /* it killed itself, or its task */
    }
    if (e == JOIST_BLOCKED) {
        env->pending[t] = 1;
        if (nr == ABI_CALL && k->th[t].sc_to != 0) {
            env->handover = joist_sched_handed(k, t, place, k->th[t].sc_to - 1u);
        } else if (nr == ABI_REPLY_RECV && answered != 0) {
            env->handover = joist_sched_handed(k, t, place, answered - 1u);
        }
        return ABI_WAITING;
    }
    /* Calls that receive return the message (Recv and ReplyRecv when a
     * caller was waiting, Wait when bits were pending). */
    if (e == JOIST_OK && (nr == ABI_RECV || nr == ABI_REPLY_RECV || nr == ABI_WAIT)) {
        outputs(k, t, e);
        return ABI_DONE;
    }
    R->r[ABI_RAX] = (uint64_t)e;
    R->r[ABI_RDX] = out;
    return ABI_DONE;
}
