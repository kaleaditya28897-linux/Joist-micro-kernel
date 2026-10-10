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
 *   nanoseconds; on every entry the thread that ran is charged for it, and
 *   before every exit the one-shot timer is armed for the next thing that
 *   can change the choice without a system call (joist_sched_next_event).
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
#define ROOT_BUDGET      4096u
/* The root task's own scheduling context (in its slot RUN_ROOT_SC): the
 * middle priority, and the whole of a 10 ms period. */
#define ROOT_PRIO        128u
#define ROOT_PERIOD_NS   10000000ull

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

static struct joist_kernel K;
static struct abi_env env;
static uint32_t cur; /* the thread running, or JOIST_MAX_THREADS */
static uint64_t resumed_at; /* when it was last entered (ns) */
static uint32_t cur_space = JOIST_MAX_CSPACES;

static struct {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
} percpu;

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

/* ---- the core's hooks ---- */

static void hk_space_new(void *ctx, uint32_t s)
{
    (void)ctx;
    if (vm_create(&vspace[s], pool_get, to_ptr, NULL) != KERR_OK) {
        die("page-table pool empty (it is sized from the core's limits)");
    }
    vspace[s].put_frame = pool_put;
    vm_share_upper(&vspace[s], paging_kernel_space());
}

static void hk_space_gone(void *ctx, uint32_t s)
{
    (void)ctx;
    if (cur_space == s) {
        /* Never free the tables CR3 points at: back to the kernel's. */
        __asm__ volatile("mov %0, %%cr3" : : "r"(paging_kernel_space()->root) : "memory");
        cur_space = JOIST_MAX_CSPACES;
    }
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
    if (cur_space == s) {
        for (uint64_t i = 0; i < npages; i++) {
            __asm__ volatile("invlpg (%0)" : : "r"((va + i) * VM_PAGE) : "memory");
        }
    }
}

/* ---- the ABI's environment ---- */

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

const char *run_start(struct frame_alloc *fa, uint64_t hhdm,
                      const struct limine_memmap_response *mm,
                      const struct limine_module_response *mods)
{
    hhdm_off = hhdm;
    for (uint32_t i = 0; i < POOL_FRAMES; i++) {
        if (frame_get(fa, &pool[i]) != KERR_OK) {
            return "out of frames for the page-table pool";
        }
    }
    npool = POOL_FRAMES;

    joist_init(&K);
    K.hooks = (struct joist_hooks){
        .space_new = hk_space_new, .space_gone = hk_space_gone, .map = hk_map, .unmap = hk_unmap};
    joist_cspace root;
    if (joist_boot(&K, ROOT_BUDGET, &root) != JOIST_OK) {
        return "the core refused to boot";
    }
    env = (struct abi_env){.k = &K, .space_of = space_of, .out = serial_sink, .ctx = NULL};

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

    /* syscall: EFER.SCE; STAR: kernel CS 0x08 (SS 0x10) on entry; LSTAR the
     * entry; SFMASK clears IF, TF, DF and AC on entry. (The way back is
     * iretq, so STAR's sysret half is unused.) */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1u);
    wrmsr(MSR_STAR, ((uint64_t)GDT_KDATA << 48) | ((uint64_t)GDT_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, (1u << 9) | (1u << 8) | (1u << 10) | (1u << 18));
    percpu.kernel_rsp = (uint64_t)(uintptr_t)(kernel_stack + sizeof kernel_stack);
    wrmsr(MSR_GS_BASE, 0);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&percpu);

    kputs("joist: root task: ");
    kputu((uint64_t)RUN_POOL_FRAMES * RUN_POOL_PAGES * VM_PAGE / (1024ull * 1024ull));
    kputs(" MiB of memory, ");
    kputu(nmods);
    kputs(" module(s); entering it at ");
    kputx(entry);
    kputs("\n");
    cur = t;
    apic_timer_oneshot(); /* from the boot tick to one-shot: armed on every exit */
    K.now = apic_clock_ns();
    resumed_at = K.now;
    run_user_trap(NULL); /* "return" to user mode: the root task starts */
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
    if (!has_handler(t)) {
        kputs("joist: task ");
        kputu(K.th[t].cs);
        kputs(" faulted (");
        kputs(trap_name(vector));
        kputs(" at ");
        kputx(pc);
        kputs(") with no handler: destroyed\n");
    }
    (void)joist_fault(&K, t, fault_kind(vector), addr, code);
}

/* Nothing can run: sleep until the next event the core names (a deadline,
 * a new period), unless there is none and no destruction is pending: then
 * no thread can ever run again, which is reported, not hidden. */
static void idle(void)
{
    for (;;) {
        joist_thread t;
        K.now = apic_clock_ns();
        joist_tick(&K, 0); /* deadlines that have passed */
        if (joist_sched_pick(&K, &t) == JOIST_OK) {
            return;
        }
        if (K.npending != 0) {
            (void)joist_continue(&K, 0);
            continue;
        }
        uint64_t next = joist_sched_next_event(&K, JOIST_MAX_THREADS);
        if (next == JOIST_FOREVER) {
            kputs("joist: no thread can ever run again (every thread waits forever): stopping\n");
            kernel_finish(6);
        }
        apic_timer_arm(next > K.now ? next - K.now : 1);
        __asm__ volatile("sti; hlt; cli" : : : "memory");
    }
}

/* Run thread t: its pending call's outputs, its space, its TLS base, and a
 * frame from its record, entered with iretq. */
static __attribute__((noreturn)) void enter(uint32_t t)
{
    abi_complete(&env, t);
    /* The one-shot timer: the next thing that changes who should run. */
    K.now = apic_clock_ns();
    uint64_t next = joist_sched_next_event(&K, t);
    if (next == JOIST_FOREVER) {
        apic_timer_disarm();
    } else {
        apic_timer_arm(next > K.now ? next - K.now : 1);
    }
    resumed_at = K.now;
    struct joist_regs *r = &K.th[t].regs;
    uint32_t s = K.th[t].cs;
    if (s != cur_space) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(vspace[s].root) : "memory");
        cur_space = s;
    }
    wrmsr(MSR_FS_BASE, r->tls < USER_TOP ? r->tls : 0);
    struct trap_frame *tf =
        (struct trap_frame *)(uintptr_t)(kernel_stack + sizeof kernel_stack - sizeof *tf);
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
    cur = t;
    resume_user(tf);
}

void run_user_trap(struct trap_frame *tf)
{
    uint32_t t = cur;
    /* Time first: the core's clock, and what the thread that ran used. */
    K.now = apic_clock_ns();
    if (t < JOIST_MAX_THREADS) {
        joist_sched_charge(&K, t, K.now - resumed_at);
        resumed_at = K.now;
    }
    if (tf != NULL && t < JOIST_MAX_THREADS && K.th[t].state != JOIST_T_FREE) {
        save(t, tf);
        if (tf->vector == SYSCALL_VECTOR) {
            int outcome = abi_syscall(&env, t);
            if (env.finished) {
                kputs("joist: root task exited with code ");
                kputu(env.exit_code);
                kputs(" (timer ticks so far: ");
                kputu(apic_ticks());
                kputs(")\n");
                kernel_finish(env.exit_code == 0 ? 0 : 3);
            }
            (void)outcome;
        } else if (tf->vector == APIC_TIMER_VECTOR) {
            run_tick();
        } else if (tf->vector < 32u) {
            fault(t, tf->vector, tf->vector == 14u ? read_cr2() : 0, tf->error, tf->rip);
        }
        /* (Other vectors: spurious; nothing to do.) */
    }
    for (;;) {
        /* Who runs is the core's decision, every time (step 5). */
        if (joist_sched_pick(&K, &t) != JOIST_OK) {
            idle();
            continue;
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
        enter(t);
    }
}

void run_tick(void)
{
    apic_timer_interrupt(); /* count it, and EOI */
    K.now = apic_clock_ns();
    joist_tick(&K, 0); /* deadlines that have passed wake their threads */
}
