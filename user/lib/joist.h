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
    J_SYS_DEBUG_WRITE = 1, J_SYS_EXIT = 2,
    J_SYS_CALL = 10, J_SYS_RECV, J_SYS_REPLY, J_SYS_REPLY_RECV, J_SYS_SIGNAL, J_SYS_WAIT,
    J_SYS_RECV_SLOTS, J_SYS_REPLY_SAVE, J_SYS_REPLY_SAVED,
    J_SYS_OBJ_CREATE = 20, J_SYS_CAP_DERIVE, J_SYS_CAP_MINT, J_SYS_CAP_DELETE, J_SYS_CAP_READ,
    J_SYS_CAP_GIVE, J_SYS_CSPACE_GROW, J_SYS_CSPACE_SHRINK,
    J_SYS_TASK_CREATE = 30, J_SYS_TASK_DESTROY, J_SYS_THREAD_SPAWN, J_SYS_THREAD_KILL,
    J_SYS_THREAD_SUSPEND, J_SYS_THREAD_RESUME, J_SYS_THREAD_ABORT, J_SYS_THREAD_SET_FAULT,
    J_SYS_TASK_SET_FAULT,
    J_SYS_FRAME_SPLIT = 40, J_SYS_MAP, J_SYS_UNMAP,
    J_SYS_SC_CREATE = 50, J_SYS_SC_BIND, J_SYS_SC_UNBIND, J_SYS_TASK_SET_MCP,
    J_SYS_CONTINUE = 60, J_SYS_TASK_REAP, J_SYS_TASK_DESCENDANT, J_SYS_NOSENDERS_NEXT,
    J_SYS_BIND_NOSENDERS, J_SYS_REQUEST_DEADNAME
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
    uint64_t caps;
    uint64_t badge;
};
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
