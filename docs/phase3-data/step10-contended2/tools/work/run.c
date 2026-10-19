/* run.c - the kernel at work (Phase 3 step 4; docs/phase3-plan.md).
 *
 * The core (kernel/core) decides; this file carries it out on x86_64:
 *
 * - One struct joist_kernel, in BSS: every object, space and thread the
 *   system will have, sized at build time (0003: no growth at run time).
 * - Address spaces: one page-table set per space, built and freed through
 *   the core's hooks, from a pool of frames reserved at boot whose size
 *   follows from the core's limits (each space a PML4; each mapping, which
 *   lies within one page table, at most three tables), so a hook never
 *   fails. Every PML4 shares the kernel half, prefilled at boot.
 * - Threads: one kernel stack per CPU. Every entry from user mode (system
 *   call, interrupt, exception) saves the thread's user registers into its
 *   record; every exit builds a frame from the record of the thread that
 *   runs next and enters it with iretq (resume_user). The kernel runs with
 *   interrupts off and keeps nothing on its stack between entries (0016).
 * - Scheduling (step 5): the core's rules (joist_sched_pick: fixed
 *   priorities, contexts with budgets, donation). Time is the TSC in
 *   nanoseconds; on every entry the thread that ran is charged for it
 *   (and for the kernel's time before it was resumed; idle time is no
 *   one's), and before every exit the one-shot timer is armed for the next
 *   thing that can change the choice without a system call
 *   (joist_sched_next_event).
 * - The root task (0013): its image is loaded through the core, a frame per
 *   page mapped into its own space, so the core's record of its address
 *   space is complete; it is given frames for a memory pool and for every
 *   other boot module, and starts with registers saying where they are. */
#include "run.h"

#include "abi.h"
#include "apic.h"
#include "elf.h"
#include "internal.h"
#include "joist/kprint.h"
#include "paging.h"
#include "serial.h"
#include "vm.h"

#define USER_LO          0x400000ull
#define USER_TOP         0x0000800000000000ull
#define USER_STACK_PAGES 8u
#ifndef ROOT_BUDGET
#define ROOT_BUDGET      4096u /* the root task's quota of objects */
#endif
/* The root task's own scheduling context (in its slot RUN_ROOT_SC): the
 * middle priority, and the whole of a 10 ms period. */
#define ROOT_PRIO        128u
#define ROOT_PERIOD_NS   10000000ull

/* Preemption points (0016; step 7): the slots of dying tasks one piece of
 * kernel work sweeps before it looks whether something more urgent is due.
 * This bounds how long the kernel keeps interrupts off for a destruction,
 * however large. 0 would mean never: a whole cascade in one go (a self-test
 * builds that kernel to show the difference). */
#ifndef RUN_PREEMPT_SLOTS
#define RUN_PREEMPT_SLOTS 32u
#endif

/* The direct switch after a Call or ReplyRecv (step 8). 0 builds a kernel
 * that always asks the scheduler: the slow path, which is the
 * specification, for the benchmark to compare against. */
#ifndef RUN_FAST_PATH
#define RUN_FAST_PATH 1
#endif
/* The sysretq way out for threads that came in by `syscall` (isr.S); 0:
 * always iretq. */
#ifndef RUN_SYSRET
#define RUN_SYSRET RUN_FAST_PATH
#endif

#define MSR_EFER           0xc0000080u
#define MSR_STAR           0xc0000081u
#define MSR_LSTAR          0xc0000082u
#define MSR_SFMASK         0xc0000084u
#define MSR_FS_BASE        0xc0000100u
#define MSR_GS_BASE        0xc0000101u
#define MSR_KERNEL_GS_BASE 0xc0000102u

/* RFLAGS a thread may set: CF, PF, AF, ZF, SF, DF, OF. IF and the reserved
 * bit 1 are always set; TF, IOPL, NT, AC and the rest never. */
#define USER_FLAGS 0xcd5u

extern uint8_t kernel_stack[16384];
void syscall_entry(void);
__attribute__((noreturn)) void resume_user(struct trap_frame *tf);
__attribute__((noreturn)) void resume_user_sysret(struct trap_frame *tf);

static struct joist_kernel K;
static struct abi_env env;

/* ---- CPUs (step 9; decision 0016) ----
 *
 * Everything the kernel shares (K, env, the page-table pool) is used under
 * one lock, taken on every entry and released on every exit and while a CPU
 * sleeps. What follows is each CPU's own. Three fields are read or written
 * by other CPUs, without the lock or while its holder waits for this CPU:
 * in_kernel, leave_space and frame (see cpu_sync and hk_off_cpu). */
#ifndef RUN_MAX_CPUS
#define RUN_MAX_CPUS 4u
#endif
_Static_assert(RUN_MAX_CPUS <= JOIST_MAX_CPUS && RUN_MAX_CPUS <= GDT_MAX_CPUS, "CPUs");
/* (The invariant-checking test build runs the core's checker in the
 * kernel, and the checker keeps tables the size of the core's on its
 * stack: tens of kilobytes with the larger table sizes. Found by that
 * build overrunning the 16 KiB stack into the records below it; there is
 * no guard page under a kernel stack yet.) */
#ifdef RUN_CHECK_INVARIANTS
#define CPU_STACK 262144u
#else
#define CPU_STACK 16384u
#endif

struct cpu {
    uint64_t kernel_rsp; /* syscall.S reads these two through GS: offsets 0 and 8 */
    uint64_t user_rsp;
    uint32_t id;
    uint32_t lapic_id;
    uint32_t cur;        /* the thread running here, or JOIST_MAX_THREADS */
    uint32_t cur_space;  /* whose page tables CR3 holds, or JOIST_MAX_CSPACES: the kernel's */
    uint64_t cur_tls;    /* what FS base holds; UINT64_MAX: not known */
    uint64_t resumed_at; /* what cur is charged from: the kernel entry before it was resumed (ns) */
    /* The moment the one-shot timer is set for (JOIST_FOREVER: off); 0 once
     * it has fired or idle has used it, so the next exit sets it again. */
    uint64_t armed_for;
    /* The user registers of the entry in progress, where another CPU can
     * take them (hk_off_cpu). */
    struct trap_frame *frame;
    /* 0 only while this CPU is in user mode: set first thing on entry,
     * cleared under the lock just before leaving. */
    volatile uint32_t in_kernel;
    /* Another CPU asks this one off its address space, on to the kernel's
     * tables (pages unmapped from it, or the space going): answered by
     * clearing it, wherever this CPU is (cpu_sync). */
    volatile uint32_t leave_space;
    uint32_t poked; /* a "choose again" interrupt is on its way here */
    /* Spaces whose translations this CPU may hold stale: the next switch
     * to one is a flushing one (PCID or not). */
    uint8_t stale[JOIST_MAX_CSPACES];
};
static struct cpu cpus[RUN_MAX_CPUS];
static uint32_t ncpus = 1;
static volatile uint32_t cpus_online;
static volatile uint32_t smp_running; /* past boot: interrupts in the kernel belong to a CPU's idle */
static __attribute__((aligned(CPU_STACK))) uint8_t cpu_stack[RUN_MAX_CPUS][CPU_STACK];

/* A CPU's kernel stack holds a pointer to its record in its top word, and
 * every stack is aligned to its size: so "which CPU am I" is one mask of
 * the stack pointer, with no GS and no MSR. (Not valid on the exception
 * stacks of NMI, machine check and double fault, which only report.) */
static inline struct cpu *this_cpu(void)
{
    uint64_t sp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    return *(struct cpu **)(uintptr_t)((sp & ~(uint64_t)(CPU_STACK - 1u)) + CPU_STACK - 8u);
}

static uint64_t stack_top(const struct cpu *c)
{
    return (uint64_t)(uintptr_t)&cpu_stack[c->id][CPU_STACK - 16u];
}

/* The kernel lock (0016): a ticket lock, so CPUs get it in the order they
 * asked. A CPU waiting for it has interrupts off and answers cpu_sync
 * between looks, or the holder, waiting for that answer, would wait for
 * ever. With one CPU there is nobody to keep out and it isn't taken. */
static void cpu_sync(struct cpu *c);

/* ---- The lock study (hand-off of 2026-10-15, for revisiting 0016) ----
 *
 * RUN_LOCK_STATS: every acquisition is timed with the TSC (asked, got,
 * released), and so are the scheduler's scans under the lock; counted per
 * CPU for acquisitions that fall in a window a root task sets with system
 * call RUN_LS_SYSCALL (an unknown number to the ABI, which refuses it as
 * usual after this has looked at it): RDI 0 sets the window [RSI, RDX) in
 * TSC counts and clears the counts, 1 closes it now and prints them.
 *
 * RUN_LOCK_MCS: a queue lock (Mellor-Crummey and Scott) in place of the
 * ticket lock: still first come, first served, but each waiter spins on a
 * line of its own, and a release writes only the next waiter's. */
#ifndef RUN_LOCK_MCS
#define RUN_LOCK_MCS 0
#endif
/* The one-shot timer, which is this CPU's own and whose write leaves a
 * virtual machine (about 6 300 cycles on the P53), is written after the
 * lock is let go on the way out; the moment is still chosen under it
 * (decision 0017; 0 builds the old order, for comparison). Why that is
 * safe: only this CPU reads or writes its timer and `armed_for`; it runs
 * with interrupts off until the return to user mode, so an interrupt or a
 * request from another CPU that comes meanwhile waits, and is then an
 * ordinary kernel entry that recomputes everything under the lock; and an
 * earlier event made due by another CPU after the unlock is that CPU's to
 * arm, as it always was (each CPU arms for the next event anywhere). */
#ifndef RUN_LOCK_TIMER_OUT
#define RUN_LOCK_TIMER_OUT 1
#endif
#define RUN_LS_SYSCALL 99u

#ifdef RUN_LOCK_STATS
#define LS_WIDTH   8u    /* TSC counts a bucket */
#define LS_BUCKETS 4096u /* and one more for everything longer */
enum { LS_FAST, LS_SLOW, LS_IDLE, LS_KINDS };                  /* how the lock was let go */
enum { LS_PICK, LS_NEXT, LS_POKE, LS_TICK, LS_SYS, LS_TIMER, LS_CR3, LS_SCANS }; /* timed under it */
static const char *const ls_kind_name[LS_KINDS] = {"fast", "slow", "idle"};
static const char *const ls_scan_name[LS_SCANS] = {"pick_on", "next_event", "poke_others",
                                                   "tick", "abi_syscall", "timer_write",
                                                   "space_switch"};
struct lstat {
    uint64_t asked, got; /* this acquisition */
    uint32_t kind;       /* how it will be let go */
    uint32_t depth;      /* (contend2 study) tickets ahead of this one when it asked */
    uint64_t qn[8], qwait[8]; /* (contend2) acquisitions and their wait, by depth */
    uint64_t n, wait, hold;
    uint64_t kn[LS_KINDS], khold[LS_KINDS];
    uint64_t sn[LS_SCANS], st[LS_SCANS];
    uint32_t hold_hist[LS_BUCKETS + 1u];
    uint32_t wait_hist[LS_BUCKETS + 1u];
} __attribute__((aligned(64)));
static struct lstat lstats[RUN_MAX_CPUS];
static uint64_t ls_from = UINT64_MAX;
static uint64_t ls_to = UINT64_MAX;

static inline uint64_t ls_tsc(void)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}

static inline int ls_in(uint64_t t)
{
    return t >= ls_from && t < ls_to;
}

static void ls_released(uint32_t cpu)
{
    uint64_t now = ls_tsc();
    struct lstat *s = &lstats[cpu];
    if (!ls_in(s->got)) {
        return;
    }
    uint64_t w = s->got - s->asked;
    uint64_t h = now - s->got;
    s->n++;
    s->wait += w;
    s->hold += h;
    s->kn[s->kind]++;
    s->qn[s->depth < 7u ? s->depth : 7u]++;
    s->qwait[s->depth < 7u ? s->depth : 7u] += w;
    s->khold[s->kind] += h;
    s->hold_hist[h / LS_WIDTH < LS_BUCKETS ? h / LS_WIDTH : LS_BUCKETS]++;
    s->wait_hist[w / LS_WIDTH < LS_BUCKETS ? w / LS_WIDTH : LS_BUCKETS]++;
}

static void ls_scan(uint32_t cpu, unsigned which, uint64_t t0)
{
    if (ls_in(t0)) {
        lstats[cpu].sn[which]++;
        lstats[cpu].st[which] += ls_tsc() - t0;
    }
}
#define LS_KIND(c, k) (lstats[(c)->id].kind = (k))
#define LS_TIME(c, which, expr)                                                                \
    __extension__({                                                                            \
        uint64_t ls_t0_ = ls_tsc();                                                            \
        __typeof__(expr) ls_r_ = (expr);                                                       \
        ls_scan((c)->id, (which), ls_t0_);                                                     \
        ls_r_;                                                                                 \
    })
#define LS_TIME_V(c, which, stmt)                                                              \
    do {                                                                                       \
        uint64_t ls_t0_ = ls_tsc();                                                            \
        stmt;                                                                                  \
        ls_scan((c)->id, (which), ls_t0_);                                                     \
    } while (0)
#else
#define LS_KIND(c, k) ((void)0)
#define LS_TIME(c, which, expr) (expr)
#define LS_TIME_V(c, which, stmt) stmt
#endif

#ifdef RUN_LOCK_DEPTH
/* (contend2 study) The lock's queue without the TSC: per round of the
 * measurement and per CPU, how many acquisitions found 0, 1, 2 or 3+ tickets
 * ahead (the holder counts) when they asked. System call 99: RDI 0 starts
 * round 0 and clears, 3 starts the next round, 1 prints and stops. */
#define LD_SEGS 20u
struct ldseg {
    uint64_t n, qn[4];
};
static struct ldseg ldsegs[RUN_MAX_CPUS][LD_SEGS] __attribute__((aligned(64)));
static volatile uint32_t ld_seg = UINT32_MAX; /* off */
#endif

#if RUN_LOCK_MCS
struct qnode {
    struct qnode *volatile next;
    volatile uint32_t locked;
} __attribute__((aligned(64)));
static struct qnode qnodes[RUN_MAX_CPUS];
static struct {
    struct qnode *volatile tail;
} __attribute__((aligned(64))) klock;

static void lock(struct cpu *c)
{
    if (ncpus == 1) {
        return;
    }
#ifdef RUN_LOCK_STATS
    lstats[c->id].asked = ls_tsc();
#endif
    struct qnode *n = &qnodes[c->id];
    n->next = NULL;
    n->locked = 1;
    struct qnode *pred = __atomic_exchange_n(&klock.tail, n, __ATOMIC_ACQ_REL);
    if (pred != NULL) {
        __atomic_store_n(&pred->next, n, __ATOMIC_RELEASE);
        while (__atomic_load_n(&n->locked, __ATOMIC_ACQUIRE)) {
            cpu_sync(c);
            __asm__ volatile("pause");
        }
    }
#ifdef RUN_LOCK_STATS
    lstats[c->id].got = ls_tsc();
#endif
}

static void unlock(void)
{
    if (ncpus == 1) {
        return;
    }
    uint32_t id = this_cpu()->id;
    struct qnode *n = &qnodes[id];
#ifdef RUN_LOCK_STATS
    ls_released(id);
#endif
    struct qnode *next = __atomic_load_n(&n->next, __ATOMIC_ACQUIRE);
    if (next == NULL) {
        struct qnode *me = n;
        if (__atomic_compare_exchange_n(&klock.tail, &me, NULL, 0, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            return; /* nobody waiting */
        }
        /* One is between its exchange and its link to this node: a few
         * instructions, with its interrupts off. (Nothing to answer
         * meanwhile: only the holder asks for cpu_sync.) */
        while ((next = __atomic_load_n(&n->next, __ATOMIC_ACQUIRE)) == NULL) {
            __asm__ volatile("pause");
        }
    }
    __atomic_store_n(&next->locked, 0, __ATOMIC_RELEASE);
}
#else
static struct {
    volatile uint32_t next;
    volatile uint32_t owner;
} klock;

static void lock(struct cpu *c)
{
    if (ncpus == 1) {
        return;
    }
#ifdef RUN_LOCK_STATS
    lstats[c->id].asked = ls_tsc();
#endif
    uint32_t me = __atomic_fetch_add(&klock.next, 1, __ATOMIC_SEQ_CST);
#ifdef RUN_LOCK_STATS
    lstats[c->id].depth = me - __atomic_load_n(&klock.owner, __ATOMIC_ACQUIRE);
#endif
#ifdef RUN_LOCK_DEPTH
    uint32_t ld_d = me - __atomic_load_n(&klock.owner, __ATOMIC_ACQUIRE);
    uint32_t ld_s = ld_seg;
    if (ld_s < LD_SEGS) {
        ldsegs[c->id][ld_s].n++;
        ldsegs[c->id][ld_s].qn[ld_d < 3u ? ld_d : 3u]++;
    }
#endif
    while (__atomic_load_n(&klock.owner, __ATOMIC_ACQUIRE) != me) {
        cpu_sync(c);
        __asm__ volatile("pause");
    }
#ifdef RUN_LOCK_STATS
    lstats[c->id].got = ls_tsc();
#endif
}

static void unlock(void)
{
    if (ncpus == 1) {
        return;
    }
#ifdef RUN_LOCK_STATS
    ls_released(this_cpu()->id);
#endif
    __atomic_store_n(&klock.owner, klock.owner + 1u, __ATOMIC_RELEASE);
}
#endif

/* ---- the page-table pool ---- */

#define POOL_FRAMES (JOIST_MAX_CSPACES + 3u * JOIST_MAX_MAPPINGS + 8u)
static uint64_t pool[POOL_FRAMES];
static uint32_t npool;
static uint64_t hhdm_off;
static struct vm_space vspace[JOIST_MAX_CSPACES];

static void *to_ptr(void *ctx, uint64_t phys)
{
    (void)ctx;
    return (void *)(uintptr_t)(hhdm_off + phys);
}

static kerr pool_get(void *ctx, uint64_t *phys)
{
    (void)ctx;
    if (npool == 0) {
        return KERR_NOMEM;
    }
    *phys = pool[--npool];
    uint64_t *p = to_ptr(NULL, *phys);
    for (unsigned i = 0; i < 512u; i++) {
        p[i] = 0;
    }
    return KERR_OK;
}

static void pool_put(void *ctx, uint64_t phys)
{
    (void)ctx;
    pool[npool++] = phys; /* never more than were taken: the pool is the only source */
}

static __attribute__((noreturn)) void die(const char *why)
{
    kputs("joist: PANIC: ");
    kputs(why);
    kputs("\n");
    kernel_finish(1);
}

/* ---- PCID: address spaces keep their TLB entries across switches ----
 *
 * With CR4.PCIDE each task's space has an identifier (its number plus one;
 * 0 is the kernel's own tables) and a switch to it needn't flush anything.
 * What then has to be flushed by hand is exactly what the old full flush
 * did silently: entries of a space that isn't the current one when a page
 * is unmapped from it, and whatever a destroyed space left under an
 * identifier a new space takes. Both mark the space stale, and the next
 * switch to a stale space is a flushing one. (The current space's unmaps
 * are flushed at once with invlpg, as before.) Without PCID (an old CPU,
 * QEMU's default model) every switch flushes, as before. */
static int pcid_on;
_Static_assert(JOIST_MAX_CSPACES < 4095u, "a space's PCID is its number plus one, in 12 bits");

static void pcid_init(int report)
{
    uint32_t a = 1;
    uint32_t b;
    uint32_t c = 0;
    uint32_t d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    (void)b;
    (void)d;
#ifndef RUN_NO_PCID
    if (c & (1u << 17)) {
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | (1ull << 17)) : "memory"); /* PCIDE */
        pcid_on = 1;
    }
#endif
    if (report) {
        kputs(pcid_on ? "joist: address spaces: PCID in use\n"
                      : "joist: address spaces: no PCID, every switch flushes\n");
    }
}

/* Make space s this CPU's current one. */
static void space_switch(struct cpu *c, uint32_t s)
{
    uint64_t cr3 = vspace[s].root;
    if (pcid_on) {
        cr3 |= s + 1u;
        if (!c->stale[s]) {
            cr3 |= 1ull << 63; /* keep its entries */
        }
    }
    c->stale[s] = 0;
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    c->cur_space = s;
}

/* Answer another CPU's request to leave the address space this one is in:
 * on to the kernel's own tables, which flushes what it held of the space
 * (it is marked stale here, so the way back flushes too). Called wherever
 * this CPU may be when asked: on every entry before the lock, while waiting
 * for the lock, and from the interrupt that wakes it from sleep. Touches
 * nothing shared. */
static void cpu_sync(struct cpu *c)
{
    if (__atomic_load_n(&c->leave_space, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(paging_kernel_space()->root) : "memory");
        c->cur_space = JOIST_MAX_CSPACES;
        __atomic_store_n(&c->leave_space, 0, __ATOMIC_RELEASE);
    }
}

/* Every other CPU that is in space s leaves it, before this returns: for
 * pages just unmapped from it (their translations must be gone before the
 * frames are used again) and for a space whose tables are about to be
 * freed. The caller holds the lock; the others answer wherever they are. */
static void others_leave(const struct cpu *me, uint32_t s)
{
    for (uint32_t i = 0; i < ncpus; i++) {
        struct cpu *o = &cpus[i];
        if (o == me) {
            continue;
        }
        o->stale[s] = 1;
        if (o->cur_space != s) {
            continue;
        }
        __atomic_store_n(&o->leave_space, 1, __ATOMIC_RELEASE);
        apic_send_ipi(o->lapic_id, APIC_IPI_SYNC); /* in user mode, or asleep: come and look */
        while (__atomic_load_n(&o->leave_space, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
        }
    }
}

/* ---- the core's hooks ---- */

static void hk_space_new(void *ctx, uint32_t s)
{
    (void)ctx;
    if (vm_create(&vspace[s], pool_get, to_ptr, NULL) != KERR_OK) {
        die("page-table pool empty (it is sized from the core's limits)");
    }
    vspace[s].put_frame = pool_put;
    vm_share_upper(&vspace[s], paging_kernel_space());
    for (uint32_t i = 0; i < ncpus; i++) {
        cpus[i].stale[s] = 1; /* whatever had this number before left entries under its PCID */
    }
}

static void hk_space_gone(void *ctx, uint32_t s)
{
    (void)ctx;
    struct cpu *c = this_cpu();
    if (c->cur_space == s) {
        /* Never free the tables CR3 points at: back to the kernel's. */
        __asm__ volatile("mov %0, %%cr3" : : "r"(paging_kernel_space()->root) : "memory");
        c->cur_space = JOIST_MAX_CSPACES;
    }
    others_leave(c, s); /* nor the tables any other CPU's CR3 points at */
    vm_destroy(&vspace[s]);
}

static void hk_map(void *ctx, uint32_t s, uint64_t va, uint64_t pa, uint64_t npages, unsigned perms)
{
    (void)ctx;
    unsigned vp = VM_USER | ((perms & JOIST_MAP_W) ? VM_W : 0u) | ((perms & JOIST_MAP_X) ? VM_X : 0u);
    if (vm_map(&vspace[s], va * VM_PAGE, pa * VM_PAGE, npages * VM_PAGE, VM_PAGE, vp) != KERR_OK) {
        die("a mapping the core allowed was refused by the page tables");
    }
}

static void hk_unmap(void *ctx, uint32_t s, uint64_t va, uint64_t npages)
{
    (void)ctx;
    if (vm_unmap(&vspace[s], va * VM_PAGE, npages * VM_PAGE) != KERR_OK) {
        die("an unmapping the core asked for was refused by the page tables");
    }
    struct cpu *c = this_cpu();
    if (c->cur_space == s) {
        for (uint64_t i = 0; i < npages; i++) {
            __asm__ volatile("invlpg (%0)" : : "r"((va + i) * VM_PAGE) : "memory");
        }
    } else {
        c->stale[s] = 1; /* its entries for these pages go at the next switch to it */
    }
    others_leave(c, s); /* and no other CPU keeps them past this point */
}

/* The IPC buffer's words, from one space to another (the core's copy_msg
 * hook). Each buffer is one page-aligned page, and a message at most a
 * page: the sender's must be mapped readable by user mode, the receiver's
 * writable, or nothing is copied. Through the direct map, with no switch
 * of address space. */
static int hk_copy_msg(void *ctx, uint32_t from_space, uint64_t from_va, uint32_t to_space,
                       uint64_t to_va, uint32_t words)
{
    (void)ctx;
    uint64_t from_pa;
    uint64_t to_pa;
    unsigned from_perms;
    unsigned to_perms;
    if (words == 0 || words > JOIST_MSG_WORDS ||
        vm_translate(&vspace[from_space], from_va, &from_pa, &from_perms) != KERR_OK ||
        vm_translate(&vspace[to_space], to_va, &to_pa, &to_perms) != KERR_OK ||
        (from_perms & VM_USER) == 0 || (to_perms & (VM_USER | VM_W)) != (VM_USER | VM_W)) {
        return 0;
    }
    const uint64_t *from = to_ptr(NULL, from_pa);
    uint64_t *to = to_ptr(NULL, to_pa);
    for (uint32_t i = 0; i < words; i++) {
        to[i] = from[i];
    }
    return 1;
}

static void save(uint32_t t, const struct trap_frame *tf);

/* The core is about to stop or destroy thread t, or read or write its
 * registers, and t is on CPU `cpu`. If that is another CPU, it must stop
 * running t now and t's registers must be in its record: bring that CPU
 * into the kernel (it stops before the lock, which this CPU holds, with
 * its user registers in the frame it has published), take them, and leave
 * it with nothing, to choose again when it gets the lock.
 *
 * What that CPU had come in for is undone, not lost: a system call it had
 * not begun is backed up to the `syscall` instruction, and a fault happens
 * again when the thread next runs. */
static void hk_off_cpu(void *ctx, uint32_t t, uint32_t cpu)
{
    (void)ctx;
    struct cpu *me = this_cpu();
    if (cpu >= ncpus || cpu == me->id) {
        return; /* this CPU's own thread: saved on the way in */
    }
    struct cpu *o = &cpus[cpu];
    if (o->cur != t) {
        return;
    }
    if (!__atomic_load_n(&o->in_kernel, __ATOMIC_ACQUIRE)) {
        apic_send_ipi(o->lapic_id, APIC_IPI_SYNC);
        while (!__atomic_load_n(&o->in_kernel, __ATOMIC_ACQUIRE)) {
            __asm__ volatile("pause");
        }
    }
    const struct trap_frame *tf = o->frame;
    save(t, tf);
    if (tf->vector == SYSCALL_VECTOR) {
        K.th[t].regs.pc -= 2; /* back to the `syscall` instruction (0f 05) */
    }
    env.exact[t] = 1; /* (the instruction will clobber RCX and R11 again; until then they count) */
    joist_sched_charge(&K, t, K.now > o->resumed_at ? K.now - o->resumed_at : 0);
    o->cur = JOIST_MAX_THREADS;
}

/* ---- the ABI's environment ---- */

static struct frame_alloc *boot_frames; /* for the counters: run_start's */

static void machine_counters(void *ctx, struct abi_counters *c)
{
    (void)ctx;
    c->table_pool = npool;
    c->frames_free = boot_frames != NULL ? boot_frames->nfree : 0;
}

static const struct vm_space *space_of(void *ctx, uint32_t s)
{
    (void)ctx;
    return &vspace[s];
}

static void serial_sink(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            serial_putc('\r');
        }
        serial_putc((char)buf[i]);
    }
}

static void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

/* ---- booting the root task (0013) ---- */

static uint32_t next_image_slot = RUN_IMAGE_FIRST;

/* One fresh, zeroed page as a frame in the root's space, mapped into it at
 * page va with perms; the page itself, through the direct map, in *page. */
static const char *root_page(struct frame_alloc *fa, uint64_t va, unsigned perms, uint8_t **page)
{
    uint64_t pa;
    if (frame_get(fa, &pa) != KERR_OK) {
        return "out of frames loading the root task";
    }
    *page = to_ptr(NULL, pa);
    for (unsigned i = 0; i < VM_PAGE; i++) {
        (*page)[i] = 0;
    }
    joist_slot slot = next_image_slot++;
    if (joist_boot_frame(&K, 0, slot, pa / VM_PAGE, 1) != JOIST_OK ||
        joist_map(&K, 0, slot, JOIST_SLOT_SELF, va / VM_PAGE, perms) != JOIST_OK) {
        return "the core refused a page of the root task";
    }
    return NULL;
}

static const char *load_root(struct frame_alloc *fa, const uint8_t *image, uint64_t size,
                             uint64_t *entry)
{
    struct elf_image im;
    if (elf_validate(image, size, USER_LO, USER_TOP, &im) != KERR_OK) {
        return "the root task image was refused by the ELF validator";
    }
    if (joist_cspace_grow(&K, 0, RUN_IMAGE_FIRST / JOIST_CNODE_LEAF) != JOIST_OK) {
        return "no leaf for the root task's image frames";
    }
    for (uint32_t i = 0; i < im.nsegments; i++) {
        const struct elf_segment *s = &im.seg[i];
        unsigned perms =
            ((s->flags & ELF_SEG_W) ? JOIST_MAP_W : 0u) | ((s->flags & ELF_SEG_X) ? JOIST_MAP_X : 0u);
        uint64_t first = s->vaddr & ~(VM_PAGE - 1u);
        for (uint64_t page = first; page < s->vaddr + s->memsz; page += VM_PAGE) {
            uint8_t *p;
            const char *e = root_page(fa, page, perms, &p);
            if (e != NULL) {
                return e;
            }
            /* The bytes of [vaddr, vaddr + filesz) that fall in this page. */
            uint64_t lo = page > s->vaddr ? page : s->vaddr;
            uint64_t hi =
                page + VM_PAGE < s->vaddr + s->filesz ? page + VM_PAGE : s->vaddr + s->filesz;
            for (uint64_t va = lo; va < hi; va++) {
                p[va - page] = image[s->offset + (va - s->vaddr)];
            }
        }
    }
    for (unsigned i = 0; i < USER_STACK_PAGES; i++) {
        uint8_t *p;
        const char *e = root_page(fa, USER_LO - (uint64_t)(i + 1u) * VM_PAGE, JOIST_MAP_W, &p);
        if (e != NULL) {
            return e;
        }
    }
    *entry = im.entry;
    return NULL;
}

/* The root task's memory pool: RUN_POOL_FRAMES frames of RUN_POOL_PAGES
 * pages, carved contiguously from the top of the largest usable range. */
static const char *give_pool(struct frame_alloc *fa, const struct limine_memmap_response *mm)
{
    uint64_t best_base = 0;
    uint64_t best_len = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        const struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length > best_len) {
            best_base = e->base;
            best_len = e->length;
        }
    }
    uint64_t want = (uint64_t)RUN_POOL_FRAMES * RUN_POOL_PAGES * VM_PAGE;
    uint64_t end = (best_base + best_len) & ~(VM_LARGE - 1u);
    if (best_len < want + VM_LARGE || end - want < best_base) {
        return "no usable range large enough for the root task's memory pool";
    }
    uint64_t base = end - want;
    if (frame_take_range(fa, base, want) != KERR_OK) {
        return "the root task's memory pool overlaps frames in use";
    }
    for (uint32_t i = 0; i < RUN_POOL_FRAMES; i++) {
        uint64_t pa = base + (uint64_t)i * RUN_POOL_PAGES * VM_PAGE;
        if (joist_boot_frame(&K, 0, RUN_POOL_FIRST + i, pa / VM_PAGE, RUN_POOL_PAGES) != JOIST_OK) {
            return "the core refused a memory-pool frame";
        }
    }
    return NULL;
}

static __attribute__((noreturn)) void schedule(struct cpu *c);

/* What every CPU sets for itself: the system-call MSRs, GS (the user's 0;
 * the kernel's, for syscall.S, this CPU's record), PCID. */
static void cpu_setup(struct cpu *c, int first)
{
    /* syscall: EFER.SCE; STAR: kernel CS 0x08 (SS 0x10) on entry, and the
     * sysret half (cpu.h); LSTAR the entry; SFMASK clears IF, TF, DF and AC
     * on entry. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1u);
    wrmsr(MSR_STAR, ((uint64_t)GDT_KDATA << 48) | ((uint64_t)GDT_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, (1u << 9) | (1u << 8) | (1u << 10) | (1u << 18));
    c->kernel_rsp = stack_top(c);
    wrmsr(MSR_GS_BASE, 0);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)c);
    pcid_init(first);
}

/* Move to a CPU's own kernel stack and go on there; never comes back.
 * Below the trap frame's place, not at the very top: the top is where
 * enter() builds the frame it leaves through (as the CPU does on an entry
 * from user mode), and a function running there would have its own frame
 * overwritten. That happened on the first exit of a CPU started here
 * (ap_run, bsp_run -> schedule -> enter), once a change made the compiler
 * keep enter's thread number on the stack (decision 0017's timer order;
 * the checking kernel's growth test, every time). */
static __attribute__((noreturn)) void on_own_stack(struct cpu *c, void (*fn)(struct cpu *))
{
    uint64_t sp = (stack_top(c) - sizeof(struct trap_frame)) & ~(uint64_t)15;
    __asm__ volatile("movq %0, %%rsp\n\tcallq *%1" : : "r"(sp), "r"(fn), "D"(c) : "memory");
    __builtin_unreachable();
}

/* A CPU after the first, on its own stack: the same set-up the first had,
 * then into the scheduler with nothing to run yet. */
static __attribute__((noreturn)) void ap_run(struct cpu *c)
{
    paging_init_ap();
    gdt_init_cpu(c->id, stack_top(c));
    idt_load();
    cpu_setup(c, 0);
    apic_init_ap();
    c->lapic_id = apic_id();
    (void)__atomic_fetch_add(&cpus_online, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&smp_running, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
    lock(c);
    schedule(c);
}

/* Where Limine starts a CPU (on Limine's stack and page tables). */
static void ap_start(struct limine_mp_info *info)
{
    on_own_stack((struct cpu *)(uintptr_t)info->extra_argument, ap_run);
}

#if defined(JOIST_SELFTEST) && JOIST_SELFTEST == 8
/* Self-test 8: a CPU's kernel stack overflows into its guard page. */
static __attribute__((noinline)) uint64_t deeper(volatile uint64_t n)
{
    volatile uint8_t pad[256];
    pad[0] = (uint8_t)n;
    return n == 0 ? pad[0] : deeper(n + 1u) + pad[0]; /* (n never comes back to 0) */
}

static __attribute__((noreturn)) void overflow(struct cpu *c)
{
    (void)c;
    (void)deeper(1);
    kernel_finish(0); /* not reached: that would be the failure */
}

void run_selftest_stack_guard(void)
{
    struct cpu *c = &cpus[0];
    *c = (struct cpu){.id = 0, .cur = JOIST_MAX_THREADS, .cur_space = JOIST_MAX_CSPACES};
    *(struct cpu **)(uintptr_t)&cpu_stack[0][CPU_STACK - 8u] = c;
    if (paging_guard((uint64_t)(uintptr_t)&cpu_stack[0][0]) != KERR_OK) {
        kputs("joist: selftest: could not make the guard page\n");
        kernel_finish(3);
    }
    kputs("joist: selftest: kernel stack overflow\n");
    on_own_stack(c, overflow);
}
#endif

static __attribute__((noreturn)) void bsp_run(struct cpu *c)
{
    __atomic_store_n(&smp_running, 1, __ATOMIC_RELEASE);
    lock(c);
    schedule(c);
}

const char *run_start(struct frame_alloc *fa, uint64_t hhdm,
                      const struct limine_memmap_response *mm,
                      const struct limine_module_response *mods,
                      const struct limine_mp_response *mp)
{
    hhdm_off = hhdm;
    /* The CPUs: this one first, then as many of the others as there is
     * room for. Each gets its record, a stack that leads back to it, and
     * its place in Limine's table to start from. */
    uint32_t found = mp != NULL ? (uint32_t)mp->cpu_count : 1u;
    ncpus = 0;
    for (uint32_t i = 0; i < found + 1u && ncpus < RUN_MAX_CPUS; i++) {
        /* i == 0: the boot CPU, wherever it is in the table; then the rest. */
        const struct limine_mp_info *info = NULL;
        if (mp != NULL) {
            if (i == 0) {
                for (uint32_t j = 0; j < found; j++) {
                    if (mp->cpus[j]->lapic_id == mp->bsp_lapic_id) {
                        info = mp->cpus[j];
                    }
                }
            } else if (mp->cpus[i - 1u]->lapic_id != mp->bsp_lapic_id) {
                info = mp->cpus[i - 1u];
            }
            if (info == NULL) {
                continue;
            }
        } else if (i != 0) {
            break;
        }
        struct cpu *c = &cpus[ncpus];
        *c = (struct cpu){.id = ncpus,
                          .lapic_id = info != NULL ? info->lapic_id : 0,
                          .cur = JOIST_MAX_THREADS,
                          .cur_space = JOIST_MAX_CSPACES,
                          .cur_tls = UINT64_MAX,
                          .in_kernel = 1};
        *(struct cpu **)(uintptr_t)&cpu_stack[ncpus][CPU_STACK - 8u] = c;
        ncpus++;
    }
#ifdef RUN_ONE_CPU
    ncpus = 1;
#endif
    /* A guard page under every kernel stack: an overflow faults, and, the
     * fault having no stack to be taken on, becomes a double fault, which
     * arrives on a stack of its own and is reported. (Found needed at step
     * 9, when the checking build overran a stack into the CPU records.)
     * The usable stack is the rest, above it. */
    for (uint32_t i = 0; i < RUN_MAX_CPUS; i++) {
        if (paging_guard((uint64_t)(uintptr_t)&cpu_stack[i][0]) != KERR_OK) {
            return "could not unmap a guard page under a kernel stack";
        }
    }
    for (uint32_t i = 0; i < POOL_FRAMES; i++) {
        if (frame_get(fa, &pool[i]) != KERR_OK) {
            return "out of frames for the page-table pool";
        }
    }
    npool = POOL_FRAMES;

    joist_init(&K);
    K.preempt = RUN_PREEMPT_SLOTS;
    K.hooks = (struct joist_hooks){
        .space_new = hk_space_new, .space_gone = hk_space_gone, .map = hk_map, .unmap = hk_unmap,
        .off_cpu = hk_off_cpu, .copy_msg = hk_copy_msg};
    joist_cspace root;
    if (joist_boot(&K, ROOT_BUDGET, &root) != JOIST_OK) {
        return "the core refused to boot";
    }
    env = (struct abi_env){.k = &K, .space_of = space_of, .out = serial_sink, .ctx = NULL,
                           .machine_counters = machine_counters};
    boot_frames = fa;
    for (uint32_t i = 0; i < JOIST_MAX_THREADS; i++) {
        env.exact[i] = 1; /* until a thread enters by a system call */
    }

    const char *e = give_pool(fa, mm);
    if (e != NULL) {
        return e;
    }
    uint32_t nmods = 0;
    for (uint64_t i = 1; i < mods->module_count && nmods < RUN_FIRST_FREE - RUN_MODULE_FIRST; i++) {
        const struct limine_file *f = mods->modules[i];
        uint64_t pa = (uint64_t)(uintptr_t)f->address - hhdm;
        uint64_t pages = (f->size + VM_PAGE - 1u) / VM_PAGE;
        if (pa % VM_PAGE != 0 ||
            joist_boot_frame(&K, root, RUN_MODULE_FIRST + nmods, pa / VM_PAGE, pages) != JOIST_OK) {
            return "the core refused a boot module's frame";
        }
        nmods++;
    }
    uint64_t entry;
    const struct limine_file *rf = mods->modules[0];
    e = load_root(fa, (const uint8_t *)rf->address, rf->size, &entry);
    if (e != NULL) {
        return e;
    }
    joist_thread t;
    if (joist_thread_create(&K, root, &t) != JOIST_OK) {
        return "no thread for the root task";
    }
    /* Its scheduling context: nothing runs without one (step 5). */
    if (joist_sc_create(&K, root, RUN_ROOT_SC, ROOT_PRIO, ROOT_PERIOD_NS, ROOT_PERIOD_NS) != JOIST_OK ||
        joist__sc_bind_thread(&K, K.cs[root].slot[RUN_ROOT_SC].obj - 1u, t) != JOIST_OK) {
        return "no scheduling context for the root task";
    }
    struct joist_regs *r = &K.th[t].regs;
    r->pc = entry;
    r->sp = USER_LO;
    r->flags = 0x202u;
    r->r[ABI_RDI] = RUN_POOL_FIRST;
    r->r[ABI_RSI] = RUN_POOL_FRAMES;
    r->r[ABI_RDX] = RUN_MODULE_FIRST;
    r->r[ABI_RCX] = nmods;
    r->r[ABI_R8] = RUN_FIRST_FREE;
    r->r[ABI_R9] = ncpus;

    gdt_set_stack(0, stack_top(&cpus[0]));
    cpu_setup(&cpus[0], 1);
    apic_timer_oneshot(); /* from the boot tick to one-shot: armed on every exit */
    cpus[0].lapic_id = apic_id();
    cpus_online = 1;
    /* Start the others (each waits at the gate, smp_running, once set up). */
    if (mp != NULL) {
        for (uint32_t i = 1; i < ncpus; i++) {
            for (uint32_t j = 0; j < found; j++) {
                struct limine_mp_info *info = mp->cpus[j];
                if (info->lapic_id == cpus[i].lapic_id) {
                    info->extra_argument = (uint64_t)(uintptr_t)&cpus[i];
                    __atomic_store_n(&info->goto_address, ap_start, __ATOMIC_SEQ_CST);
                }
            }
        }
        for (uint64_t spin = 0; __atomic_load_n(&cpus_online, __ATOMIC_ACQUIRE) < ncpus; spin++) {
            if (spin > 2000000000ull) {
                return "a CPU didn't start";
            }
            __asm__ volatile("pause");
        }
    }
    kputs("joist: cpus in use: ");
    kputu(ncpus);
    kputs(ncpus > 1 ? " (one kernel lock, decision 0016)\n" : "\n");
    kputs("joist: root task: ");
    kputu((uint64_t)RUN_POOL_FRAMES * RUN_POOL_PAGES * VM_PAGE / (1024ull * 1024ull));
    kputs(" MiB of memory, ");
    kputu(nmods);
    kputs(" module(s); entering it at ");
    kputx(entry);
    kputs("\n");
    (void)t; /* the scheduler finds it */
    K.now = apic_clock_ns();
    on_own_stack(&cpus[0], bsp_run); /* into the scheduler: the root task starts */
}

/* ---- in and out of user mode ---- */

static void save(uint32_t t, const struct trap_frame *tf)
{
    struct joist_regs *r = &K.th[t].regs;
    r->pc = tf->rip;
    r->sp = tf->rsp;
    r->flags = tf->rflags;
    r->r[ABI_RAX] = tf->rax;
    r->r[ABI_RBX] = tf->rbx;
    r->r[ABI_RCX] = tf->rcx;
    r->r[ABI_RDX] = tf->rdx;
    r->r[ABI_RSI] = tf->rsi;
    r->r[ABI_RDI] = tf->rdi;
    r->r[ABI_RBP] = tf->rbp;
    r->r[ABI_R8] = tf->r8;
    r->r[ABI_R9] = tf->r9;
    r->r[ABI_R10] = tf->r10;
    r->r[ABI_R11] = tf->r11;
    r->r[ABI_R12] = tf->r12;
    r->r[ABI_R13] = tf->r13;
    r->r[ABI_R14] = tf->r14;
    r->r[ABI_R15] = tf->r15;
}

static uint64_t fault_kind(uint64_t v)
{
    switch (v) {
    case 14:
        return JOIST_FAULT_PAGE;
    case 13:
        return JOIST_FAULT_GP;
    case 6:
        return JOIST_FAULT_UD;
    case 0:
    case 16:
    case 19:
        return JOIST_FAULT_MATH;
    default:
        return JOIST_FAULT_OTHER;
    }
}

/* Is a fault of thread t going to find a handler (0009)? */
static int has_handler(uint32_t t)
{
    return K.th[t].fault_slot != 0 || K.cs[K.th[t].cs].fault_slot != 0;
}

static void fault(uint32_t t, uint64_t vector, uint64_t addr, uint64_t code, uint64_t pc)
{
    int root = K.root == K.th[t].cs + 1u;
    if (root && !has_handler(t)) {
        /* The root task faulted with nobody to tell: the run is over (as in
         * Phase 2, whose self-tests check exactly this report). */
        kputs("joist: root task fault: ");
        kputs(trap_name(vector));
        kputs(" at ");
        kputx(pc);
        if (vector == 14u) {
            kputs(", address ");
            kputx(addr);
        }
        kputs("\n");
        kernel_finish(4);
    }
    /* Whether there is a handler to tell is the core's finding (a slot can
     * be named and hold nothing usable): with none it has destroyed the
     * task, or the root's thread, and that is reported, not silent. */
    uint32_t cs = K.th[t].cs;
    if (joist_fault(&K, t, fault_kind(vector), addr, code) == JOIST_E_PEER) {
        kputs(root ? "joist: a root task thread" : "joist: task ");
        if (!root) {
            kputu(cs);
        }
        kputs(" faulted (");
        kputs(trap_name(vector));
        kputs(" at ");
        kputx(pc);
        kputs(") with no handler: destroyed\n");
    }
}

/* The clock only goes forward, whichever CPU reads it. */
static void clock_now(void)
{
    uint64_t now = apic_clock_ns();
    if (now > K.now) {
        K.now = now;
    }
}

/* After anything that may have changed who should run where (the core
 * says so: K.resched): every other CPU that would now choose differently
 * is interrupted, once, to choose again. */
static void poke_others(const struct cpu *me)
{
    if (!K.resched) {
        return;
    }
    K.resched = 0;
    for (uint32_t i = 0; i < ncpus; i++) {
        struct cpu *o = &cpus[i];
        if (o != me && !o->poked && joist_sched_better(&K, o->id)) {
            o->poked = 1;
            apic_send_ipi(o->lapic_id, APIC_IPI_RESCHED);
        }
    }
}

/* Nothing for this CPU to run: sleep until the next event the core names
 * (a deadline, a new period) or another CPU's interrupt, with the lock
 * released. If nothing will ever happen (no event, no destruction pending,
 * every other CPU idle too), no thread can ever run again, which is
 * reported, not hidden. Returns with a thread chosen. */
static uint32_t idle(struct cpu *c)
{
    c->cur = JOIST_MAX_THREADS;
    for (;;) {
        joist_thread t;
        clock_now();
        LS_TIME_V(c, LS_TICK, joist_tick(&K, 0)); /* deadlines that have passed */
        K.resched = 1;     /* and periods that have begun */
        if (LS_TIME(c, LS_PICK, joist_sched_pick_on(&K, c->id, &t)) == JOIST_OK) {
            return t;
        }
        if (K.npending != 0) {
            (void)joist_continue(&K, RUN_PREEMPT_SLOTS); /* a piece; then look again */
            continue;
        }
        LS_TIME_V(c, LS_POKE, poke_others(c));
        uint64_t next = LS_TIME(c, LS_NEXT, joist_sched_next_event(&K, JOIST_MAX_THREADS));
        if (next == JOIST_FOREVER) {
            int alone = 1;
            for (uint32_t i = 0; i < ncpus; i++) {
                alone &= cpus[i].cur == JOIST_MAX_THREADS;
            }
            if (alone) {
                kputs("joist: no thread can ever run again (every thread waits forever): stopping\n");
                kernel_finish(6);
            }
            apic_timer_disarm(); /* another CPU's interrupt will wake this one */
        } else {
            apic_timer_arm(next > K.now ? next - K.now : 1);
        }
        c->armed_for = 0;
        LS_KIND(c, LS_IDLE);
        unlock();
        __asm__ volatile("sti; hlt; cli" : : : "memory");
        lock(c);
        c->poked = 0;
    }
}

/* Destruction left pending (a task's capability table is swept a piece at
 * a time): every kernel entry does one piece and no more, so it always
 * ends, and nothing (an interrupt's wake-up least of all) is held up by
 * more than a piece. Whoever wants it done sooner calls `continue`, a piece
 * a call, on its own time; idle does the rest.
 *
 * (Not: go on for as long as the thread that entered still runs. An
 * interrupt that lands on a high-priority thread would then have the whole
 * cascade swept in that thread's name. The destruction test found it.) */
static void sweep_pending(void)
{
    if (K.npending != 0) {
        (void)joist_continue(&K, RUN_PREEMPT_SLOTS);
    }
}

/* Set this CPU's one-shot timer for `next` (JOIST_FOREVER: off). */
static void arm_timer(struct cpu *c, uint64_t next)
{
#ifdef RUN_LOCK_STATS
    uint64_t ls_t0 = ls_tsc();
#endif
    if (next == JOIST_FOREVER) {
        apic_timer_disarm();
    } else {
        uint64_t now = apic_clock_ns();
        apic_timer_arm(next > now ? next - now : 1);
    }
    c->armed_for = next;
#ifdef RUN_LOCK_STATS
    ls_scan(c->id, LS_TIMER, ls_t0);
#endif
}

/* Run thread t: its pending call's outputs, its space, its TLS base, and a
 * frame from its record, entered with iretq. */
static __attribute__((noreturn)) void enter(struct cpu *c, uint32_t t, int handed)
{
    abi_complete(&env, t);
    /* The one-shot timer: the next thing that changes who should run.
     * The thread is charged from the last kernel entry (K.now), not from
     * here: the kernel's time between an entry and the exit that follows
     * is paid for by the thread that exit resumes, so no time is free and
     * a thread that only makes system calls still spends its budget. */
    /* (After a direct switch the timer stays as it is: the core has
     * vouched that the next event is no earlier than the one it is set
     * for. At worst it fires early, and the next exit sets it right.) */
    uint64_t next = handed ? c->armed_for : LS_TIME(c, LS_NEXT, joist_sched_next_event(&K, t));
    /* Only when the moment changes: a budget's end stays where it is from
     * call to call while one context runs (as through a call to a server on
     * donated time), and a timer write is the dearest thing on this path.
     * Written here, under the lock, only in the old order; normally after
     * the unlock, below (RUN_LOCK_TIMER_OUT). */
    int rearm = next != c->armed_for;
    if (!RUN_LOCK_TIMER_OUT && rearm) {
        arm_timer(c, next);
    }
    c->resumed_at = K.now;
    struct joist_regs *r = &K.th[t].regs;
    uint32_t s = K.th[t].cs;
    if (s != c->cur_space || c->stale[s]) {
        LS_TIME_V(c, LS_CR3, space_switch(c, s));
    }
    /* The TLS base: written only when it changes (the write is slow, and
     * a thread can't change it behind the kernel's back: FSGSBASE is off). */
    uint64_t tls = r->tls < USER_TOP ? r->tls : 0;
    if (tls != c->cur_tls) {
        wrmsr(MSR_FS_BASE, tls);
        c->cur_tls = tls;
    }
    struct trap_frame *tf = (struct trap_frame *)(uintptr_t)(stack_top(c) - sizeof *tf);
    tf->rip = r->pc;
    tf->cs = GDT_UCODE | 3u;
    tf->rflags = (r->flags & USER_FLAGS) | 0x202u;
    tf->rsp = r->sp;
    tf->ss = GDT_UDATA | 3u;
    tf->vector = 0;
    tf->error = 0;
    tf->rax = r->r[ABI_RAX];
    tf->rbx = r->r[ABI_RBX];
    tf->rcx = r->r[ABI_RCX];
    tf->rdx = r->r[ABI_RDX];
    tf->rsi = r->r[ABI_RSI];
    tf->rdi = r->r[ABI_RDI];
    tf->rbp = r->r[ABI_RBP];
    tf->r8 = r->r[ABI_R8];
    tf->r9 = r->r[ABI_R9];
    tf->r10 = r->r[ABI_R10];
    tf->r11 = r->r[ABI_R11];
    tf->r12 = r->r[ABI_R12];
    tf->r13 = r->r[ABI_R13];
    tf->r14 = r->r[ABI_R14];
    tf->r15 = r->r[ABI_R15];
    c->cur = t;
#ifdef RUN_CHECK_INVARIANTS
    /* A test build: the core's whole state checked before every exit to
     * user mode, under the lock. Whatever two CPUs could do to it between
     * them shows here, at the first exit after. */
    {
        static char why[160];
        if (joist_check_invariants(&K, why, sizeof why) != 0) {
            kputs("joist: INVARIANT VIOLATED on CPU ");
            kputu(c->id);
            kputs(": ");
            kputs(why);
            kputs("\n");
            kernel_finish(2);
        }
        for (uint32_t i = 0; i < ncpus; i++) {
            /* And the kernel's own record of who is where agrees with the
             * core's (a CPU with nothing may still be marked: it has yet
             * to choose). */
            if (cpus[i].cur < JOIST_MAX_THREADS && K.on_cpu[i] != cpus[i].cur + 1u) {
                kputs("joist: INVARIANT VIOLATED: CPU ");
                kputu(i);
                kputs(" runs thread ");
                kputu(cpus[i].cur);
                kputs(", and the core has ");
                kputu(K.on_cpu[i]);
                kputs(" (a thread plus one) there; seen from CPU ");
                kputu(c->id);
                kputs(", in the kernel: ");
                kputu(cpus[i].in_kernel);
                kputs("\n");
                for (uint32_t j = 0; j < ncpus; j++) {
                    kputs("  CPU ");
                    kputu(j);
                    kputs(": runs ");
                    kputu(cpus[j].cur);
                    kputs(", core has ");
                    kputu(K.on_cpu[j]);
                    kputs(", in kernel ");
                    kputu(cpus[j].in_kernel);
                    kputs("\n");
                }
                kputs("  entering thread ");
                kputu(t);
                kputs(handed ? " by the direct switch\n" : " by the scheduler\n");
                kernel_finish(2);
            }
        }
    }
#endif
    int quick = RUN_SYSRET && !env.exact[t];
    /* Out of the kernel: said while the lock is still held (hk_off_cpu
     * relies on that), then the lock goes, and nothing shared is touched
     * after it. */
    __atomic_store_n(&c->in_kernel, 0, __ATOMIC_RELEASE);
    LS_KIND(c, handed ? LS_FAST : LS_SLOW);
    unlock();
    if (RUN_LOCK_TIMER_OUT && rearm) {
        arm_timer(c, next);
    }
    if (quick) {
        resume_user_sysret(tf); /* it came in by `syscall`: out by sysretq */
    }
    resume_user(tf);
}

#ifdef RUN_LOCK_STATS
/* The lock study's system call (above, RUN_LOCK_STATS); under the lock. */
static void ls_put(const char *what, uint64_t v)
{
    kputs(what);
    kputu(v);
}

static uint64_t ls_quantile(const uint32_t *h, uint64_t n, uint64_t part, uint64_t of)
{
    uint64_t want = n * part / of;
    want = want == 0 ? 1 : want;
    uint64_t seen = 0;
    for (unsigned i = 0; i <= LS_BUCKETS; i++) {
        seen += h[i];
        if (seen >= want) {
            return (i + 1u) * LS_WIDTH;
        }
    }
    return 0;
}

static uint32_t ls_hold_all[LS_BUCKETS + 1u];
static uint32_t ls_wait_all[LS_BUCKETS + 1u];

static void ls_op(uint64_t op, uint64_t from, uint64_t to)
{
    if (op == 0) {
        for (uint32_t i = 0; i < ncpus; i++) {
            struct lstat *s = &lstats[i];
            uint64_t asked = s->asked;
            uint64_t got = s->got;
            uint32_t kind = s->kind;
            __builtin_memset(s, 0, sizeof *s);
            s->asked = asked;
            s->got = got;
            s->kind = kind;
        }
        ls_from = from;
        ls_to = to;
        return;
    }
    uint64_t now = ls_tsc();
    uint64_t end = now < ls_to ? now : ls_to;
    uint64_t span = end > ls_from ? end - ls_from : 0;
    ls_to = now < ls_to ? now : ls_to; /* closed */
    uint64_t n = 0;
    for (unsigned b = 0; b <= LS_BUCKETS; b++) {
        ls_hold_all[b] = 0;
        ls_wait_all[b] = 0;
    }
    ls_put("lockstats: caller cpu ", this_cpu()->id);
    kputs("\n");
    ls_put("lockstats: window ", span);
    ls_put(" tsc counts, cpus ", ncpus);
    ls_put(", lock ", RUN_LOCK_MCS ? 1u : 0u);
    kputs(RUN_LOCK_MCS ? " (mcs)\n" : " (ticket)\n");
    for (uint32_t i = 0; i < ncpus; i++) {
        struct lstat *s = &lstats[i];
        ls_put("lockstats: cpu ", i);
        ls_put(" acq ", s->n);
        ls_put(" wait ", s->wait);
        ls_put(" hold ", s->hold);
        ls_put(" hold_p50 ", ls_quantile(s->hold_hist, s->n, 1, 2));
        ls_put(" hold_p99 ", ls_quantile(s->hold_hist, s->n, 99, 100));
        ls_put(" wait_p50 ", ls_quantile(s->wait_hist, s->n, 1, 2));
        ls_put(" wait_p99 ", ls_quantile(s->wait_hist, s->n, 99, 100));
        for (unsigned k = 0; k < LS_KINDS; k++) {
            kputs(" ");
            kputs(ls_kind_name[k]);
            ls_put(" ", s->kn[k]);
            ls_put("/", s->khold[k]);
        }
        for (unsigned k = 0; k < LS_SCANS; k++) {
            kputs(" ");
            kputs(ls_scan_name[k]);
            ls_put(" ", s->sn[k]);
            ls_put("/", s->st[k]);
        }
        kputs(" depth");
        for (unsigned k = 0; k < 8u; k++) {
            ls_put(" ", s->qn[k]);
            ls_put("/", s->qwait[k]);
        }
        kputs("\n");
        n += s->n;
        for (unsigned b = 0; b <= LS_BUCKETS; b++) {
            ls_hold_all[b] += s->hold_hist[b];
            ls_wait_all[b] += s->wait_hist[b];
        }
    }
    ls_put("lockstats: all acq ", n);
    ls_put(" hold_p50 ", ls_quantile(ls_hold_all, n, 1, 2));
    ls_put(" hold_p99 ", ls_quantile(ls_hold_all, n, 99, 100));
    ls_put(" wait_p50 ", ls_quantile(ls_wait_all, n, 1, 2));
    ls_put(" wait_p99 ", ls_quantile(ls_wait_all, n, 99, 100));
    ls_put(" (bucket ", LS_WIDTH);
    kputs(" tsc counts)\n");
}
#endif

#ifdef RUN_LOCK_DEPTH
/* (contend2 study) RUN_LOCK_DEPTH's system call; under the lock. */
static void ld_op(uint64_t op)
{
    if (op == 0) {
        __builtin_memset(ldsegs, 0, sizeof ldsegs);
        ld_seg = 0;
        return;
    }
    if (op == 3) {
        ld_seg = ld_seg + 1u < LD_SEGS ? ld_seg + 1u : ld_seg;
        return;
    }
    if (op != 1) {
        return;
    }
    ld_seg = UINT32_MAX;
    kputs("lockdepth: caller cpu ");
    kputu(this_cpu()->id);
    kputs("\n");
    for (uint32_t sg = 0; sg < LD_SEGS; sg++) {
        for (uint32_t i = 0; i < ncpus; i++) {
            struct ldseg *d = &ldsegs[i][sg];
            kputs("lockdepth: seg ");
            kputu(sg);
            kputs(" cpu ");
            kputu(i);
            kputs(" n ");
            kputu(d->n);
            kputs(" depth");
            for (unsigned k = 0; k < 4u; k++) {
                kputs(" ");
                kputu(d->qn[k]);
            }
            kputs("\n");
        }
    }
}
#endif

void run_user_trap(struct trap_frame *tf)
{
    struct cpu *c = this_cpu();
    /* In the kernel, with the user's registers where another CPU can find
     * them: before anything else, and before the lock. */
    c->frame = tf;
    __atomic_store_n(&c->in_kernel, 1, __ATOMIC_RELEASE);
    cpu_sync(c);
    lock(c);
    c->poked = 0;
    uint32_t t = c->cur; /* JOIST_MAX_THREADS: taken off this CPU meanwhile (hk_off_cpu) */
    /* Time first: the core's clock, and what the thread that ran used. */
    clock_now();
    if (t < JOIST_MAX_THREADS) {
        joist_sched_charge(&K, t, K.now - c->resumed_at);
        c->resumed_at = K.now;
    }
    uint64_t v = tf->vector;
    if (v == APIC_TIMER_VECTOR) {
        apic_timer_interrupt(); /* count it, and EOI */
        c->armed_for = 0;
        LS_TIME_V(c, LS_TICK, joist_tick(&K, 0)); /* deadlines that have passed wake their threads */
        K.resched = 1;     /* and a period that has begun may be anyone's */
    } else if (v == APIC_IPI_RESCHED || v == APIC_IPI_SYNC) {
        apic_eoi(); /* being here is the whole of it */
    }
    if (t < JOIST_MAX_THREADS && K.th[t].state != JOIST_T_FREE) {
        save(t, tf);
        env.exact[t] = v != SYSCALL_VECTOR; /* an interrupt or a fault: every register counts */
        if (v == SYSCALL_VECTOR) {
            /* What the core's direct switch takes for granted about the
             * caller, checked here before the call changes anything: it
             * was the scheduler's choice (every thread the kernel resumes
             * is), nothing has come due since (the clock hasn't reached
             * what the timer is set for), and no destruction is pending
             * (every entry owes that a piece, below). */
#ifdef RUN_LOCK_STATS
            if (K.th[t].regs.r[ABI_RAX] == RUN_LS_SYSCALL) {
                ls_op(K.th[t].regs.r[ABI_RDI], K.th[t].regs.r[ABI_RSI], K.th[t].regs.r[ABI_RDX]);
            }
#endif
#ifdef RUN_LOCK_DEPTH
            if (K.th[t].regs.r[ABI_RAX] == RUN_LS_SYSCALL) {
                ld_op(K.th[t].regs.r[ABI_RDI]);
            }
#endif
            int may_hand =
                RUN_FAST_PATH && K.npending == 0 && c->armed_for != 0 && K.now < c->armed_for;
            /* The run queues are held for the call and settled after the
             * choice (joist_sched_hold): a thread handed this CPU below
             * never goes on a queue and off it again. */
            joist_sched_hold(&K);
            int outcome = LS_TIME(c, LS_SYS, abi_syscall(&env, t));
            if (env.finished) {
                kputs("joist: root task exited with code ");
                kputu(env.exit_code);
                kputs(" (timer ticks so far: ");
                kputu(apic_ticks());
                kputs(")\n");
                kernel_finish(env.exit_code == 0 ? 0 : 3);
            }
            (void)outcome;
            if (may_hand && env.handover < JOIST_MAX_THREADS && K.npending == 0 &&
                K.th[env.handover].regs.pc < USER_TOP) {
                /* The fast path: no scheduler, no timer. The other thread
                 * takes this CPU in the caller's place, so no other CPU's
                 * choice has changed either. */
                joist_sched_on(&K, c->id, env.handover);
                joist_sched_settle(&K);
                K.resched = 0;
                enter(c, env.handover, 1);
            }
            joist_sched_settle(&K);
            if (may_hand && K.npending == 0 && c->cur == t && K.th[t].regs.pc < USER_TOP &&
                joist_sched_stays(&K, c->id, t)) {
                /* The same thread goes on: the core says the scheduler
                 * would choose it again and change nothing, no CPU need
                 * choose again, and so the call made nothing that could
                 * bring the next event sooner. (What other CPUs made since this one set its
                 * timer is theirs: each sets its own for the next event
                 * anywhere when it leaves, as after a direct switch.) So
                 * no scheduler, no timer: a call that changes
                 * nothing (a refused one, a signal nobody waits for) holds
                 * the lock for its own work only (decision 0017's
                 * measurements: the scheduler was 43 % of such a hold). */
                enter(c, t, 1);
            }
        } else if (v < 32u) {
            fault(t, v, v == 14u ? read_cr2() : 0, tf->error, tf->rip);
        }
        /* (Other vectors: an interrupt; dealt with above.) */
    }
    sweep_pending();
    schedule(c);
}

/* Choose and run: the end of every kernel entry but the fast path's. */
static __attribute__((noreturn)) void schedule(struct cpu *c)
{
    for (;;) {
        /* Who runs is the core's decision, every time (step 5). */
        joist_thread t;
        if (LS_TIME(c, LS_PICK, joist_sched_pick_on(&K, c->id, &t)) != JOIST_OK) {
            t = idle(c);
        }
        /* The way out goes to a user-half address only: an address past it
         * would fault in the kernel (Phase 2's sysret hazard, kept). */
        if (K.th[t].regs.pc >= USER_TOP) {
            if (K.root == K.th[t].cs + 1u && !has_handler(t)) {
                kputs("joist: root task refused: syscall returning to non-canonical ");
                kputx(K.th[t].regs.pc);
                kputs("\n");
                kernel_finish(3);
            }
            fault(t, 13u, 0, 0, K.th[t].regs.pc);
            continue;
        }
        LS_TIME_V(c, LS_POKE, poke_others(c)); /* this CPU has chosen; who else should? */
        enter(c, t, 0);
    }
}

void run_kernel_irq(uint64_t vector)
{
    if (!__atomic_load_n(&smp_running, __ATOMIC_ACQUIRE)) {
        apic_timer_interrupt(); /* the boot's own ticks */
        return;
    }
    /* A sleeping CPU's interrupt: nothing shared is touched here (the lock
     * isn't held); the sleeper looks at everything when it has it. */
    struct cpu *c = this_cpu();
    if (vector == APIC_TIMER_VECTOR) {
        apic_timer_interrupt();
        c->armed_for = 0;
    } else {
        apic_eoi();
        cpu_sync(c);
    }
}
