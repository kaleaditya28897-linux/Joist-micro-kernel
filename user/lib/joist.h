/* joist.h - the Joist system calls from user mode (Phase 3 step 4;
 * docs/abi.md, *Encoding*; the kernel side is kernel/sys/abi.h, and
 * kernel/test/test_abi_user.c checks at compile time that the numbers here
 * are the kernel's).
 *
 * Header only, freestanding. One raw call, j_syscall, loads and returns
 * every register the ABI uses; the wrappers below are that call with the
 * registers named. Results are J_OK (0) or a J_E_* code. */
#ifndef JOIST_USER_JOIST_H
#define JOIST_USER_JOIST_H

#include <stddef.h>
#include <stdint.h>

/* Result codes (docs/abi.md). */
enum {
    J_OK = 0, J_E_RANGE, J_E_EMPTY, J_E_OCCUPIED, J_E_RIGHTS, J_E_NOMEM, J_E_BADGE, J_E_TYPE,
    J_E_DEAD, J_E_TIMEOUT, J_E_GONE, J_E_PEER, J_E_NOREPLY, J_E_STATE, J_E_ABORTED
};

/* Object types, rights, mapping permissions. */
enum { J_ENDPOINT = 1, J_NOTIFICATION, J_TASK, J_THREAD, J_REPLY, J_SCHED, J_FRAME };
#define J_R_SEND    0x1u
#define J_R_RECV    0x2u
#define J_R_KILL    0x4u /* on frames: obj_kill (0022) */
#define J_R_GRANT   0x8u
#define J_R_ALL     0xfu
#define J_R_CONTROL J_R_SEND
#define J_R_WRITE   J_R_RECV
#define J_MAP_W     1u
#define J_MAP_X     2u

#define J_FOREVER   UINT64_MAX
#define J_MS        1000000ull /* times are in nanoseconds */
#define J_SLOT_NONE 0xffffffffu
#define J_SLOT_SELF 0xfffffffeu
#define J_PAGE      4096u
#define J_MAP_SPAN  512u /* pages one mapping may span */

/* Call numbers. */
enum {
    J_SYS_DEBUG_WRITE = 1, J_SYS_EXIT = 2, J_SYS_DEBUG_COUNTERS = 3,
    J_SYS_CALL = 10, J_SYS_RECV, J_SYS_REPLY, J_SYS_REPLY_RECV, J_SYS_SIGNAL, J_SYS_WAIT,
    J_SYS_RECV_SLOTS, J_SYS_REPLY_SAVE, J_SYS_REPLY_SAVED,
    J_SYS_OBJ_CREATE = 20, J_SYS_CAP_DERIVE, J_SYS_CAP_MINT, J_SYS_CAP_DELETE, J_SYS_CAP_READ,
    J_SYS_CAP_GIVE, J_SYS_CSPACE_GROW, J_SYS_CSPACE_SHRINK, J_SYS_CAP_COPY_BATCH, J_SYS_OBJ_KILL,
    J_SYS_TASK_CREATE = 30, J_SYS_TASK_DESTROY, J_SYS_THREAD_SPAWN, J_SYS_THREAD_KILL,
    J_SYS_THREAD_SUSPEND, J_SYS_THREAD_RESUME, J_SYS_THREAD_ABORT, J_SYS_THREAD_SET_FAULT,
    J_SYS_TASK_SET_FAULT,
    J_SYS_FRAME_SPLIT = 40, J_SYS_MAP, J_SYS_UNMAP,
    J_SYS_SC_CREATE = 50, J_SYS_SC_BIND, J_SYS_SC_UNBIND, J_SYS_TASK_SET_MCP, J_SYS_YIELD,
    J_SYS_CONTINUE = 60, J_SYS_TASK_REAP, J_SYS_TASK_DESCENDANT, J_SYS_NOSENDERS_NEXT,
    J_SYS_BIND_NOSENDERS, J_SYS_REQUEST_DEADNAME,
    J_SYS_THREAD_READ_REGS = 70, J_SYS_THREAD_WRITE_REGS, J_SYS_IPC_BUFFER
};

/* Every register a call takes or returns. */
struct j_regs {
    uint64_t rax, rdi, rsi, rdx, r10, r8, r9, r12, r13, r14, r15;
};

static inline void j_syscall(struct j_regs *r)
{
    register uint64_t r10 __asm__("r10") = r->r10;
    register uint64_t r8 __asm__("r8") = r->r8;
    register uint64_t r9 __asm__("r9") = r->r9;
    register uint64_t r12 __asm__("r12") = r->r12;
    register uint64_t r13 __asm__("r13") = r->r13;
    register uint64_t r14 __asm__("r14") = r->r14;
    register uint64_t r15 __asm__("r15") = r->r15;
    __asm__ volatile("syscall"
                     : "+a"(r->rax), "+D"(r->rdi), "+S"(r->rsi), "+d"(r->rdx), "+r"(r10), "+r"(r8),
                       "+r"(r9), "+r"(r12), "+r"(r13), "+r"(r14), "+r"(r15)
                     :
                     : "rcx", "r11", "memory");
    r->r10 = r10;
    r->r8 = r8;
    r->r9 = r9;
    r->r12 = r12;
    r->r13 = r13;
    r->r14 = r14;
    r->r15 = r15;
}

/* A call with up to four arguments and nothing but a result. */
static inline uint64_t j_sys4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    struct j_regs r = {.rax = nr, .rdi = a0, .rsi = a1, .rdx = a2, .r10 = a3};
    j_syscall(&r);
    return r.rax;
}

/* A message: four registers, and the capabilities it carries or brought
 * (docs/abi.md: count in bits 0-1, then per position a 16-bit slot and a
 * 4-bit rights mask). badge is set on receipt. */
struct j_msg {
    uint64_t r[4];
    uint64_t caps;  /* the capabilities, and the buffer's word count (J_WORDS) */
    uint64_t badge;
};
/* The IPC buffer (docs/abi.md, *Messages*): up to J_MSG_WORDS words of the
 * thread's buffer go with a message; their number shares R9 with the
 * capabilities. On receipt, J_WORDS(caps) words arrived in the receiver's
 * buffer, and J_LOST(caps) says words were sent that couldn't be placed. */
#define J_MSG_WORDS       512u
#define J_WORDS_SET(n)    ((uint64_t)(n) << 48)
#define J_WORDS(caps)     ((unsigned)(((caps) >> 48) & 0x3ffu))
#define J_LOST(caps)      ((unsigned)((caps) >> 63))
/* Register this thread's IPC buffer: a page-aligned page of its own, or
 * NULL for none. */
static inline uint64_t j_ipc_buffer(void *page);
#define J_CAPS1(slot, mask) (1u | (uint64_t)(slot) << 2 | (uint64_t)(mask) << 18)
#define J_CAPS_COUNT(caps)  ((unsigned)((caps) & 3u))
#define J_CAPS_SLOT(caps, i) ((unsigned)(((caps) >> (2u + 20u * (i))) & 0xffffu))

static inline uint64_t j_ipc(uint64_t nr, uint64_t slot, uint64_t timeout, struct j_msg *m)
{
    struct j_regs r = {.rax = nr, .rdi = slot, .rsi = timeout, .r9 = m->caps,
                       .r12 = m->r[0], .r13 = m->r[1], .r14 = m->r[2], .r15 = m->r[3]};
    j_syscall(&r);
    m->r[0] = r.r12;
    m->r[1] = r.r13;
    m->r[2] = r.r14;
    m->r[3] = r.r15;
    m->caps = r.r9;
    m->badge = r.rdx;
    return r.rax;
}

/* IPC: m goes out and the answer comes back in it. */
static inline uint64_t j_call(uint64_t ep, struct j_msg *m, uint64_t timeout)
{
    return j_ipc(J_SYS_CALL, ep, timeout, m);
}
static inline uint64_t j_recv(uint64_t ep, struct j_msg *m, uint64_t timeout)
{
    return j_ipc(J_SYS_RECV, ep, timeout, m);
}
static inline uint64_t j_reply(struct j_msg *m)
{
    return j_ipc(J_SYS_REPLY, 0, 0, m);
}
static inline uint64_t j_reply_recv(uint64_t ep, struct j_msg *m, uint64_t timeout)
{
    return j_ipc(J_SYS_REPLY_RECV, ep, timeout, m);
}
static inline uint64_t j_recv_slots(uint64_t s0, uint64_t s1, uint64_t n)
{
    return j_sys4(J_SYS_RECV_SLOTS, s0, s1, n, 0);
}

static inline uint64_t j_ipc_buffer(void *page)
{
    return j_sys4(J_SYS_IPC_BUFFER, (uint64_t)(uintptr_t)page, 0, 0, 0);
}

/* Deferred replies: the reply capability into a reply object, and the
 * answer through it later. */
static inline uint64_t j_reply_save(uint64_t robj)
{
    return j_sys4(J_SYS_REPLY_SAVE, robj, 0, 0, 0);
}
static inline uint64_t j_reply_saved(uint64_t robj, struct j_msg *m)
{
    return j_ipc(J_SYS_REPLY_SAVED, robj, 0, m);
}

/* Notifications: *bits is the word a Wait returns. */
static inline uint64_t j_signal(uint64_t n)
{
    return j_sys4(J_SYS_SIGNAL, n, 0, 0, 0);
}
static inline uint64_t j_wait(uint64_t n, uint64_t timeout, uint64_t *bits)
{
    struct j_msg m = {0};
    uint64_t e = j_ipc(J_SYS_WAIT, n, timeout, &m);
    *bits = e == J_OK ? m.badge : 0;
    return e;
}

/* Lifetime notifications (docs/abi.md): no-senders on an endpoint one
 * holds RECV on, the badges it queued one at a time (0 when none is), and
 * a dead-name registration on any endpoint. */
static inline uint64_t j_bind_nosenders(uint64_t ep, uint64_t n)
{
    return j_sys4(J_SYS_BIND_NOSENDERS, ep, n, 0, 0);
}
static inline uint64_t j_nosenders_next(uint64_t ep, uint64_t *badge)
{
    struct j_regs r = {.rax = J_SYS_NOSENDERS_NEXT, .rdi = ep};
    j_syscall(&r);
    *badge = r.rax == J_OK ? r.rdx : 0;
    return r.rax;
}
static inline uint64_t j_request_deadname(uint64_t ep, uint64_t n)
{
    return j_sys4(J_SYS_REQUEST_DEADNAME, ep, n, 0, 0);
}

/* Capabilities and objects. */
static inline uint64_t j_obj_create(uint64_t type, uint64_t dst)
{
    return j_sys4(J_SYS_OBJ_CREATE, type, dst, 0, 0);
}
static inline uint64_t j_cap_derive(uint64_t src, uint64_t dst, uint64_t mask)
{
    return j_sys4(J_SYS_CAP_DERIVE, src, dst, mask, 0);
}
static inline uint64_t j_cap_mint(uint64_t src, uint64_t dst, uint64_t mask, uint64_t badge)
{
    return j_sys4(J_SYS_CAP_MINT, src, dst, mask, badge);
}
static inline uint64_t j_cap_delete(uint64_t slot)
{
    return j_sys4(J_SYS_CAP_DELETE, slot, 0, 0, 0);
}
static inline uint64_t j_cap_give(uint64_t task, uint64_t src, uint64_t dst, uint64_t mask)
{
    return j_sys4(J_SYS_CAP_GIVE, task, src, dst, mask);
}
/* The batched copy for fork (0022): each capability in slots [first,
 * first + count) (count <= J_COPY_BATCH) with GRANT goes to the same slot of
 * the task, rights & mask, badge kept; receive rights only with
 * J_COPY_RECV. *done is how many slots were dealt with (count unless it
 * stopped at an occupied slot, J_E_OCCUPIED, or a leaf the task lacks,
 * J_E_RANGE: go on from first + *done), *skipped a bit per slot skipped. */
#define J_COPY_BATCH 32u
#define J_COPY_RECV  1u
static inline uint64_t j_cap_copy_batch(uint64_t task, uint64_t first, uint64_t count,
                                        uint64_t mask, uint64_t flags, uint64_t *done,
                                        uint64_t *skipped)
{
    struct j_regs r = {.rax = J_SYS_CAP_COPY_BATCH, .rdi = task, .rsi = first, .rdx = count,
                       .r10 = mask, .r8 = flags};
    j_syscall(&r);
    *done = r.rdx;
    *skipped = r.r8;
    return r.rax;
}
/* Kill the object (a frame) in slot: every capability to it, anywhere, then
 * fails J_E_GONE; needs J_R_KILL (0022). */
static inline uint64_t j_obj_kill(uint64_t slot)
{
    return j_sys4(J_SYS_OBJ_KILL, slot, 0, 0, 0);
}
/* Add leaf `leaf` to the caller's capability table: 128 more slots, from
 * slot leaf * 128. */
static inline uint64_t j_cspace_grow(uint64_t leaf)
{
    return j_sys4(J_SYS_CSPACE_GROW, leaf, 0, 0, 0);
}
/* A frame capability's size in pages, or 0 if slot isn't a frame. */
static inline uint64_t j_frame_pages(uint64_t slot)
{
    struct j_regs r = {.rax = J_SYS_CAP_READ, .rdi = slot};
    j_syscall(&r);
    return r.rax == J_OK ? r.r10 : 0;
}

/* Tasks, threads, memory. */
static inline uint64_t j_task_create(uint64_t donation, uint64_t dst)
{
    return j_sys4(J_SYS_TASK_CREATE, donation, dst, 0, 0);
}
static inline uint64_t j_thread_spawn(uint64_t task, uint64_t dst, uint64_t pc, uint64_t sp)
{
    return j_sys4(J_SYS_THREAD_SPAWN, task, dst, pc, sp);
}
static inline uint64_t j_frame_split(uint64_t frame, uint64_t at, uint64_t dst)
{
    return j_sys4(J_SYS_FRAME_SPLIT, frame, at, dst, 0);
}
static inline uint64_t j_map(uint64_t frame, uint64_t task, uint64_t va_page, uint64_t perms)
{
    return j_sys4(J_SYS_MAP, frame, task, va_page, perms);
}
static inline uint64_t j_unmap(uint64_t task, uint64_t va_page)
{
    return j_sys4(J_SYS_UNMAP, task, va_page, 0, 0);
}

/* Thread control (0010): everything through a thread capability. */
static inline uint64_t j_thread_kill(uint64_t thread)
{
    return j_sys4(J_SYS_THREAD_KILL, thread, 0, 0, 0);
}
static inline uint64_t j_thread_suspend(uint64_t thread)
{
    return j_sys4(J_SYS_THREAD_SUSPEND, thread, 0, 0, 0);
}
static inline uint64_t j_thread_resume(uint64_t thread)
{
    return j_sys4(J_SYS_THREAD_RESUME, thread, 0, 0, 0);
}
static inline uint64_t j_thread_abort(uint64_t thread)
{
    return j_sys4(J_SYS_THREAD_ABORT, thread, 0, 0, 0);
}
/* A thread's registers: one record (the kernel's struct joist_regs). */
enum { J_RAX, J_RBX, J_RCX, J_RDX, J_RSI, J_RDI, J_RBP, J_R8, J_R9, J_R10, J_R11, J_R12, J_R13,
       J_R14, J_R15, J_NREGS = 16 }; /* the last is spare */
struct j_thread_regs {
    uint64_t pc, sp, flags, tls;
    uint64_t r[J_NREGS];
};
static inline uint64_t j_thread_read_regs(uint64_t thread, struct j_thread_regs *out)
{
    return j_sys4(J_SYS_THREAD_READ_REGS, thread, (uint64_t)(uintptr_t)out, 0, 0);
}
static inline uint64_t j_thread_write_regs(uint64_t thread, const struct j_thread_regs *in)
{
    return j_sys4(J_SYS_THREAD_WRITE_REGS, thread, (uint64_t)(uintptr_t)in, 0, 0);
}

/* Faults (0009): the handler is a slot in the thread's (or task's) own
 * space holding a send capability; a fault is a Call on it. The message:
 * r[0] the kind, r[1] the address, r[2] the error code, and a capability to
 * the faulting thread. The reply's r[0]: J_FAULT_RESUME, or anything else
 * to end the thread. */
enum { J_FAULT_PAGE = 1, J_FAULT_GP, J_FAULT_UD, J_FAULT_MATH, J_FAULT_OTHER };
#define J_FAULT_RESUME 0u
static inline uint64_t j_thread_set_fault(uint64_t thread, uint64_t handler)
{
    return j_sys4(J_SYS_THREAD_SET_FAULT, thread, handler, 0, 0);
}
static inline uint64_t j_task_set_fault(uint64_t task, uint64_t handler)
{
    return j_sys4(J_SYS_TASK_SET_FAULT, task, handler, 0, 0);
}
static inline uint64_t j_task_destroy(uint64_t task)
{
    return j_sys4(J_SYS_TASK_DESTROY, task, 0, 0, 0);
}

/* The first live strict descendant of the caller with an id >= from, in
 * *id; E_EMPTY if there is none. */
static inline uint64_t j_task_descendant(uint64_t from, uint64_t *id)
{
    struct j_regs r = {.rax = J_SYS_TASK_DESCENDANT, .rdi = from};
    j_syscall(&r);
    *id = r.rdx;
    return r.rax;
}

/* Go on with destruction left pending (docs/abi.md, *Task destruction*): at
 * most `budget` slots, and never more than the kernel's own limit. *more is
 * whether any is left. */
static inline uint64_t j_continue(uint64_t budget, int *more)
{
    struct j_regs r = {.rax = J_SYS_CONTINUE, .rdi = budget};
    j_syscall(&r);
    *more = r.rdx != 0;
    return r.rax;
}

/* Scheduling contexts: a priority (0-255) and a budget per period, in
 * nanoseconds. A thread runs only on one: its own, or one lent by a caller. */
static inline uint64_t j_sc_create(uint64_t dst, uint64_t prio, uint64_t budget, uint64_t period)
{
    return j_sys4(J_SYS_SC_CREATE, dst, prio, budget, period);
}
static inline uint64_t j_sc_bind(uint64_t sc, uint64_t thread)
{
    return j_sys4(J_SYS_SC_BIND, sc, thread, 0, 0);
}
static inline uint64_t j_sc_unbind(uint64_t sc)
{
    return j_sys4(J_SYS_SC_UNBIND, sc, 0, 0, 0);
}

/* What the kernel has in use (the root task only; kernel/sys/abi.h,
 * struct abi_counters). J_OK, J_E_RIGHTS or J_E_RANGE. */
struct j_counters {
    uint64_t objects, threads, spaces, leaves, mappings, pending, table_pool, frames_free;
    uint64_t rq_walks; /* run-queue insert steps past a list's end, so far */
};
static inline uint64_t j_debug_counters(struct j_counters *out)
{
    return j_sys4(J_SYS_DEBUG_COUNTERS, (uint64_t)(uintptr_t)out, 0, 0, 0);
}

/* Give up the rest of this turn: behind equals (decision 0014). */
static inline uint64_t j_yield(void)
{
    return j_sys4(J_SYS_YIELD, 0, 0, 0, 0);
}

/* Debug calls (Phase 2's convention: bytes written, or a negative code). */
static inline uint64_t j_debug_write(const void *p, uint64_t n)
{
    return j_sys4(J_SYS_DEBUG_WRITE, (uint64_t)(uintptr_t)p, n, 0, 0);
}
static inline __attribute__((noreturn)) void j_exit(uint64_t code)
{
    (void)j_sys4(J_SYS_EXIT, code, 0, 0, 0);
    for (;;) {
    }
}

/* Print a string, and a number in decimal: enough for test programs. */
static inline void j_puts(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    (void)j_debug_write(s, n);
}
static inline void j_putu(uint64_t v)
{
    char buf[20];
    size_t i = sizeof buf;
    do {
        buf[--i] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    (void)j_debug_write(&buf[i], sizeof buf - i);
}

#endif /* JOIST_USER_JOIST_H */
