/* user.c - the root task in ring 3 (Phase 2 step 6).
 *
 * Its address space: a new PML4 whose kernel half (entries 256-511) is copied
 * from the kernel's, so the kernel is mapped in every task. (Kernel mappings
 * made later in a new top-level slot would not propagate; Phase 2 makes none
 * after this, and Phase 3's address-space code takes over.) Its segments come
 * from elf_validate's description, each page in its own fresh frame with the
 * segment's permissions plus user access; its stack is the USER_STACK_PAGES
 * pages just below the image. System calls arrive through syscall.S. */
#include "user.h"

#include "apic.h"
#include "cpu.h"
#include "elf.h"
#include "joist/kprint.h"
#include "paging.h"
#include "serial.h"
#include "syscall.h"

/* The stack sits just below the image, so the image may use the user half up
 * to its very top (step 7 places code at the top to test the sysret guard). */
#define USER_LO          0x400000ull
#define USER_TOP_        0x0000800000000000ull
#define USER_STACK_TOP   USER_LO
#define USER_STACK_PAGES 8u

#define MSR_EFER           0xc0000080u
#define MSR_STAR           0xc0000081u
#define MSR_LSTAR          0xc0000082u
#define MSR_SFMASK         0xc0000084u
#define MSR_GS_BASE        0xc0000101u
#define MSR_KERNEL_GS_BASE 0xc0000102u

extern uint8_t kernel_stack[16384];
void syscall_entry(void);
__attribute__((noreturn)) void user_enter(uint64_t rip, uint64_t rsp);

/* The per-CPU block syscall.S reaches through GS: kernel stack top, then a
 * slot for the user RSP. */
static struct {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
} percpu;

struct frames_ctx {
    struct frame_alloc *fa;
    uint64_t hhdm;
};
static struct frames_ctx fctx;
static struct vm_space task;

static kerr zeroed_frame(void *ctx, uint64_t *phys)
{
    struct frames_ctx *c = ctx;
    kerr k = frame_get(c->fa, phys);
    if (k == KERR_OK) {
        uint64_t *p = (uint64_t *)(uintptr_t)(c->hhdm + *phys);
        for (unsigned i = 0; i < 512u; i++) {
            p[i] = 0;
        }
    }
    return k;
}

static void *to_ptr(void *ctx, uint64_t phys)
{
    const struct frames_ctx *c = ctx;
    return (void *)(uintptr_t)(c->hhdm + phys);
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

/* Map one segment page by page into fresh frames and copy its file bytes. */
static const char *load_segment(const struct elf_segment *s, const uint8_t *image)
{
    unsigned perms =
        VM_USER | ((s->flags & ELF_SEG_W) ? VM_W : 0u) | ((s->flags & ELF_SEG_X) ? VM_X : 0u);
    uint64_t first = s->vaddr & ~(VM_PAGE - 1u);
    uint64_t end = s->vaddr + s->memsz;
    for (uint64_t page = first; page < end; page += VM_PAGE) {
        uint64_t pa;
        if (zeroed_frame(&fctx, &pa) != KERR_OK) {
            return "out of frames loading the root task";
        }
        if (vm_map(&task, page, pa, VM_PAGE, VM_PAGE, perms) != KERR_OK) {
            return "mapping a root task segment failed";
        }
        /* The bytes of [s->vaddr, s->vaddr + filesz) that fall in this page. */
        uint64_t lo = page > s->vaddr ? page : s->vaddr;
        uint64_t hi = page + VM_PAGE < s->vaddr + s->filesz ? page + VM_PAGE : s->vaddr + s->filesz;
        uint8_t *dst = to_ptr(&fctx, pa);
        for (uint64_t va = lo; va < hi; va++) {
            dst[va - page] = image[s->offset + (va - s->vaddr)];
        }
    }
    return NULL;
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

static struct sys_env env;

uint64_t syscall_handler(struct syscall_frame *f)
{
    uint64_t r = sys_dispatch(&env, f->rax, f->rdi, f->rsi);
    if (env.exited) {
        kputs("joist: root task exited with code ");
        kputu(env.exit_code);
        kputs(" (timer ticks so far: ");
        kputu(apic_ticks());
        kputs(")\n");
        kernel_finish(env.exit_code == 0 ? 0 : 3);
    }
    /* The sysret hazard: returning to a non-canonical RIP would fault in
     * ring 0 on the user's stack. It can only happen for a syscall at the
     * very top of the user half; end the task instead. */
    if (f->rcx >= 0x0000800000000000ull) {
        kputs("joist: root task refused: syscall returning to non-canonical ");
        kputx(f->rcx);
        kputs("\n");
        kernel_finish(3);
    }
    return r;
}

const char *user_run_root(struct frame_alloc *fa, uint64_t hhdm, const uint8_t *image,
                          uint64_t size)
{
    struct elf_image im;
    if (elf_validate(image, size, USER_LO, USER_TOP_, &im) != KERR_OK) {
        return "the root task image was refused by the ELF validator";
    }
    fctx = (struct frames_ctx){.fa = fa, .hhdm = hhdm};
    if (vm_create(&task, zeroed_frame, to_ptr, &fctx) != KERR_OK) {
        return "no frame for the root task's PML4";
    }
    /* The kernel half, shared. */
    const uint64_t *kpml4 = to_ptr(&fctx, paging_kernel_space()->root);
    uint64_t *tpml4 = to_ptr(&fctx, task.root);
    for (unsigned i = 256; i < 512u; i++) {
        tpml4[i] = kpml4[i];
    }
    for (uint32_t i = 0; i < im.nsegments; i++) {
        const char *e = load_segment(&im.seg[i], image);
        if (e != NULL) {
            return e;
        }
    }
    for (unsigned i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t pa;
        uint64_t va = USER_STACK_TOP - (uint64_t)(i + 1u) * VM_PAGE;
        if (zeroed_frame(&fctx, &pa) != KERR_OK ||
            vm_map(&task, va, pa, VM_PAGE, VM_PAGE, VM_W | VM_USER) != KERR_OK) {
            return "mapping the root task's stack failed";
        }
    }
    env = (struct sys_env){.task = &task, .out = serial_sink, .ctx = NULL};

    /* syscall: EFER.SCE; STAR: kernel CS 0x08 (SS 0x10) on entry, sysret base
     * 0x10 so user SS = 0x18|3 and CS = 0x20|3; LSTAR the entry; SFMASK
     * clears IF, TF, DF and AC on entry. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1u);
    wrmsr(MSR_STAR, ((uint64_t)GDT_KDATA << 48) | ((uint64_t)GDT_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, (1u << 9) | (1u << 8) | (1u << 10) | (1u << 18));
    percpu.kernel_rsp = (uint64_t)(uintptr_t)(kernel_stack + sizeof kernel_stack);
    wrmsr(MSR_GS_BASE, 0);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)&percpu);

    kputs("joist: entering the root task at ");
    kputx(im.entry);
    kputs("\n");
    __asm__ volatile("mov %0, %%cr3" : : "r"(task.root) : "memory");
    user_enter(im.entry, USER_STACK_TOP);
}
