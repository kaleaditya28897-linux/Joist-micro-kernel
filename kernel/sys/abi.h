/* SPDX-License-Identifier: GPL-3.0-only */
/* abi.h - the system-call ABI: registers to core operations (Phase 3 step
 * 4; docs/abi.md, *Encoding*; plan choice 8).
 *
 * A thread's user registers are saved in its record (struct joist_regs: pc,
 * sp, flags, tls, r[] = RAX RBX RCX RDX RSI RDI RBP R8-R15) on every entry,
 * and restored from it on every exit, so the dispatcher works on the record
 * alone and needs no CPU access: it is host-tested and fuzzed (§8.1).
 *
 * In:  RAX the call number; RDI, RSI, RDX, R10, R8 its arguments; R12-R15
 *      the four message registers (0001 amendment); R9 the capabilities a
 *      message carries, packed (ABI_CAPS_*).
 * Out: RAX the result (a joist_err, so 0 is success); RDX a badge or a
 *      returned value; R12-R15 a received message; R9 the receive slots
 *      capabilities landed in, packed. Every other register is the caller's.
 *      The `syscall` instruction itself clobbers RCX and R11.
 *
 * A call that blocks (Call, Recv, ReplyRecv, Wait) leaves the thread waiting
 * with its outputs unwritten: abi_complete writes them from the thread's
 * record when it runs again.
 *
 * Calls 1 and 2 keep Phase 2's debug convention (docs/phase2-plan.md):
 * debug_write returns the bytes written or a negated SYS_E_* code, exit ends
 * the run (from the root task) or the calling thread; an unknown call number
 * returns -SYS_E_NOSYS and changes nothing else. */
#ifndef JOIST_SYS_ABI_H
#define JOIST_SYS_ABI_H

#include "joist/core.h"
#include "syscall.h"
#include "vm.h"

#include <stddef.h>
#include <stdint.h>

/* Register indexes into struct joist_regs.r. */
enum {
    ABI_RAX = 0, ABI_RBX, ABI_RCX, ABI_RDX, ABI_RSI, ABI_RDI, ABI_RBP,
    ABI_R8, ABI_R9, ABI_R10, ABI_R11, ABI_R12, ABI_R13, ABI_R14, ABI_R15
};

/* Call numbers. */
#define ABI_DEBUG_WRITE      1u
#define ABI_EXIT             2u
#define ABI_DEBUG_COUNTERS   3u  /* address of a struct abi_counters to fill (the root task only) */
#define ABI_CALL             10u /* ep, timeout; message, caps */
#define ABI_RECV             11u /* ep, timeout */
#define ABI_REPLY            12u /* message, caps */
#define ABI_REPLY_RECV       13u /* ep, timeout; message, caps */
#define ABI_SIGNAL           14u /* notification */
#define ABI_WAIT             15u /* notification, timeout -> RDX the bits */
#define ABI_RECV_SLOTS       16u /* slot0, slot1, n */
#define ABI_REPLY_SAVE       17u /* reply object */
#define ABI_REPLY_SAVED      18u /* reply object; message, caps */
#define ABI_OBJ_CREATE       20u /* type, dst */
#define ABI_CAP_DERIVE       21u /* src, dst, mask */
#define ABI_CAP_MINT         22u /* src, dst, mask, badge */
#define ABI_CAP_DELETE       23u /* slot */
#define ABI_CAP_READ         24u /* slot -> RDX type | rights << 8, R8 badge, R10 frame pages */
#define ABI_CAP_GIVE         25u /* task, src, dst, mask */
#define ABI_CSPACE_GROW      26u /* leaf */
#define ABI_CSPACE_SHRINK    27u /* leaf */
#define ABI_CAP_COPY_BATCH   28u /* task, first, count, mask, flags -> RDX done, R8 skipped (0022) */
#define ABI_OBJ_KILL         29u /* slot (0022) */
#define ABI_TASK_CREATE      30u /* donation, dst */
#define ABI_TASK_DESTROY     31u /* task */
#define ABI_THREAD_SPAWN     32u /* task, dst, pc, sp: the new thread starts there */
#define ABI_THREAD_KILL      33u /* thread */
#define ABI_THREAD_SUSPEND   34u /* thread */
#define ABI_THREAD_RESUME    35u /* thread */
#define ABI_THREAD_ABORT     36u /* thread */
#define ABI_THREAD_SET_FAULT 37u /* thread, handler slot, flags (JOIST_FAULT_NO_THREAD_CAP) */
#define ABI_TASK_SET_FAULT   38u /* task, handler slot, flags */
#define ABI_FRAME_SPLIT      40u /* frame, at, dst */
#define ABI_MAP              41u /* frame, page offset, pages, task, va page, perms */
#define ABI_UNMAP            42u /* task, va page, pages (joist_unmap_range) */
#define ABI_MAP_REPLACE      43u /* as ABI_MAP; r12 the old frame, r13 its page offset */
#define ABI_PROTECT          44u /* task, va page, pages, perms */
#define ABI_SPACE_DONATE     45u /* task, frame */
#define ABI_MAP_QUERY        46u /* task, va page, pages, frame (or none); r8 the buffer (0020) */
#define ABI_SC_CREATE        50u /* dst, prio, budget, period */
#define ABI_SC_BIND          51u /* context, thread */
#define ABI_SC_UNBIND        52u /* context */
#define ABI_TASK_SET_MCP     53u /* task, mcp */
#define ABI_YIELD            54u /* (decision 0014) */
#define ABI_SC_BIND_FALLBACK 55u /* context, thread (decision 0025) */
#define ABI_CLOCK            56u /* -> RDX the time in ns since boot, R10 the CPUs (Phase 5 step 7) */
#define ABI_CONTINUE         60u /* budget */
#define ABI_TASK_REAP        61u /* task id */
#define ABI_TASK_DESCENDANT  62u /* from -> RDX the task id */
#define ABI_NOSENDERS_NEXT   63u /* endpoint -> RDX the badge */
#define ABI_BIND_NOSENDERS   64u /* endpoint, notification */
#define ABI_REQUEST_DEADNAME 65u /* endpoint, notification */

/* Regions (decision 0030). */
#define ABI_REGION_CREATE   80u /* task, lo page, tables, ceiling, r8 dst */
#define ABI_REGION_ADD      81u /* region, frame, page offset, pages -> RDX the extent */
#define ABI_REGION_MAP      82u /* region, extent, page in it, pages; r8 va page, r9 perms */
#define ABI_REGION_UNMAP    83u /* region, va page, pages */
#define ABI_REGION_PROTECT  84u /* region, va page, pages, perms */

#define ABI_THREAD_READ_REGS  70u /* thread, address of a struct joist_regs to fill */
#define ABI_THREAD_WRITE_REGS 71u /* thread, address of a struct joist_regs to load */
#define ABI_IPC_BUFFER        72u /* page-aligned address of the calling thread's IPC buffer, or 0 */

/* Calls 1 and 2, and unknown numbers, use Phase 2's dispatcher and result
 * codes (syscall.h: SYS_E_*, DEBUG_WRITE_MAX), unchanged. */

/* Capabilities in R9: bits 0-1 the count, then per position (16 bits slot,
 * 4 bits rights mask) at bits 2 and 22. A received slot that didn't arrive
 * reads ABI_CAPS_NONE. */
/* The IPC buffer's words, in R9 beside the capabilities: their number in
 * bits 48-57 (0 to JOIST_MSG_WORDS, sending and received), and on receipt
 * bit 63 set if words were sent that couldn't be placed. */
#define ABI_LEN_SHIFT 48u
#define ABI_LEN_MASK  0x3ffu
#define ABI_LOST_BIT  (1ull << 63)
#define ABI_CAPS_SLOT_BITS 16u
#define ABI_CAPS_NONE      0xffffu
uint64_t abi_caps_pack(const struct joist_msg *m);
void abi_caps_unpack(uint64_t v, struct joist_msg *m);

/* What the kernel holds right now (Phase 3's exit: the memory-growth test).
 * Counts of what is in use in the core's fixed tables, and what is free in
 * the kernel's own pools. A test reads it before and after work that should
 * leave nothing behind. */
struct abi_counters {
    uint64_t objects;     /* live objects */
    uint64_t threads;     /* thread records in use */
    uint64_t spaces;      /* capability spaces alive (tasks, the root, fixtures) */
    uint64_t leaves;      /* capability-table leaves taken from the pool */
    uint64_t mappings;    /* page entries present, in every space (0020) */
    uint64_t pending;     /* destructions not yet finished */
    uint64_t table_pool;  /* page-table frames free in the kernel's pool */
    uint64_t frames_free; /* frames free in the boot frame allocator */
    uint64_t rq_walks;    /* run-queue insert steps past a list's end, so far (choice 9) */
    uint64_t table_pages; /* lent pages the spaces' page tables use (0021) */
};

struct abi_env {
    struct joist_kernel *k;
    /* The address space of space s, for the checked copies of user memory
     * (debug_write's bytes, a register record). */
    const struct vm_space *(*space_of)(void *ctx, uint32_t s);
    /* The machine's half of abi_counters (its page-table pool and frame
     * allocator); NULL in the host tests, where those are 0. */
    void (*machine_counters)(void *ctx, struct abi_counters *c);
    /* Where debug_write's bytes go. */
    void (*out)(void *ctx, const uint8_t *buf, size_t n);
    void *ctx;
    /* The CPUs the kernel runs on, for clock (0 counts as 1: the host
     * tests). */
    uint32_t ncpus;
    /* Set by exit from the root task: the run is over. */
    int finished;
    uint64_t exit_code;
    /* The direct switch (step 8): set by every abi_syscall. After a Call or
     * ReplyRecv that handed the caller's scheduling context to another
     * thread, that thread, if the core is certain the scheduler would choose
     * it (joist_sched_handed, whose conditions on the caller the kernel
     * vouches for: see run.c); otherwise JOIST_MAX_THREADS. */
    uint32_t handover;
    /* A thread that must be resumed with every register as recorded: one
     * just spawned, or whose registers were written through a capability.
     * (A thread that entered by a system call may otherwise get RCX and R11
     * back clobbered, as the `syscall` instruction itself leaves them; the
     * kernel uses that for a quicker way out. It sets and clears this for
     * the ways in it sees: run.c.) */
    uint8_t exact[JOIST_MAX_THREADS];
    /* A blocked call per thread, waiting for abi_complete. */
    uint8_t pending[JOIST_MAX_THREADS];
};

/* Outcome of a call. */
enum { ABI_DONE, ABI_WAITING, ABI_GONE };

/* Run the system call thread t made. ABI_DONE: its outputs are in its
 * registers and it can go on; ABI_WAITING: it is blocked; ABI_GONE: it no
 * longer exists (exit, or it destroyed itself or its task). */
int abi_syscall(struct abi_env *env, uint32_t t);

/* Thread t is about to run: if a blocking call of its is done, write the
 * call's outputs (result, badge, message, capabilities) into its registers.
 * Also forgets the pending call of a thread slot that is free now. */
void abi_complete(struct abi_env *env, uint32_t t);

#endif /* JOIST_SYS_ABI_H */
