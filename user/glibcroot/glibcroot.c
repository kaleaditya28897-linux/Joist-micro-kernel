/* SPDX-License-Identifier: GPL-3.0-only */
/* glibcroot - starts the first glibc program on Joist (Phase 5 step 7, the
 * design's item 8; item 11 d's first run). Module 0 is mm, module 1 a
 * static program linked with glibc's sysdeps/joist (user/glibctest, built
 * by tools/phase5/glibc-test.sh).
 *
 * It plays the Hurd's exec server: the program's image loaded, its task
 * handed to mm (ADOPT, which puts mm's capability in the task's MM_SLOT),
 * the slots glibc's start-up fixes filled (the task's own capability in 1,
 * its first thread's in 2: glibc/sysdeps/joist/joist-thread.h), and its
 * stack laid out as glibc's _hurd_startup reads it (sysdeps/joist/
 * hurdstartup.c): argc, argv, a null pointer, envp, a null pointer, then a
 * struct hurd_startup_data, whose arrays (the descriptor table, the init
 * ports, the init ints) are in a page of their own. No init port is given:
 * with no proc server glibc starts no signal thread.
 *
 * Then it waits for the program to end itself (exit: task_terminate, the
 * task's own TASK_DESTROY), and sees that mm forgot it and has every page
 * back. The program reports its own checks ("glibc: ... ok"). */
#define TEST_NAME "glibcroot"
#include "../lib/testroot.h"
#define HOST_POOL_PAGES 512u /* one of the root's frames, its most; the program's tree may have half */
#include "../lib/mmhost.h"

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free);

/* The program's stack: STACK_TOP down, the start-up block in the page at
 * STACK_TOP (sp points at its first word), the arrays in a page below the
 * stack, a guard gap between. All below the image (CHILD_LO) and below
 * 4 GiB: glibc's anywhere window starts there (joist-vm.h). */
#define GT_STACK_PAGES 32u
#define GT_BLOCK_VA    (CHILD_LO - 0x21000ull)                          /* 0x3df000 */
#define GT_STACK_LO    (GT_BLOCK_VA - GT_STACK_PAGES * (uint64_t)J_PAGE) /* 0x3bf000 */
#define GT_ARRAYS_VA   (GT_STACK_LO - 0x10000ull)                       /* 0x3af000 */
#define GT_DONATION    512u /* glibc's objects: a notification and two badges a thread, a
                             * context, 16 futex buckets, its name table's leaves */
#define GT_PRIO        100u
#define GT_WAIT_NS     (60u * 1000u * J_MS)

/* Hurd's numbers (hurd/hurd_types.h). */
#define INIT_PORT_MAX 6u
#define INIT_INT_MAX  5u
#define INIT_UMASK    0u

/* struct hurd_startup_data on x86-64 (hurd/hurdstartup.h): int flags;
 * mach_port_t *dtable; mach_msg_type_number_t dtablesize; mach_port_t
 * *portarray; ...size; int *intarray; ...size; vm_address_t stack_base;
 * vm_size_t stack_size; vm_address_t phdr; vm_size_t phdrsz; vm_address_t
 * user_entry: each in 8 bytes, 96 in all. */
enum { HSD_FLAGS, HSD_DTABLE, HSD_DTABLESIZE, HSD_PORTARRAY, HSD_PORTARRAYSIZE, HSD_INTARRAY,
       HSD_INTARRAYSIZE, HSD_STACK_BASE, HSD_STACK_SIZE, HSD_PHDR, HSD_PHDRSZ, HSD_USER_ENTRY,
       HSD_WORDS };

static uint8_t page[J_PAGE];

static void put64(uint64_t off, uint64_t v)
{
    for (unsigned i = 0; i < 8u; i++) {
        page[off + i] = (uint8_t)(v >> (8u * i));
    }
}

static void put32(uint64_t off, uint32_t v)
{
    for (unsigned i = 0; i < 4u; i++) {
        page[off + i] = (uint8_t)(v >> (8u * i));
    }
}

static uint64_t get(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = n; i > 0; i--) {
        v = v << 8 | p[i - 1u];
    }
    return v;
}

/* A string at `off` in the block; its address in the program. */
static uint64_t put_str(uint64_t *off, const char *s)
{
    uint64_t at = GT_BLOCK_VA + *off;
    while (*s != '\0') {
        page[(*off)++] = (uint8_t)*s++;
    }
    page[(*off)++] = 0;
    return at;
}

static void clear(void)
{
    for (unsigned i = 0; i < J_PAGE; i++) {
        page[i] = 0;
    }
}

/* Where the program's own image has its program headers (the module is
 * mapped here at MOD_VA + 2 MiB by ld_image): the PT_LOAD that holds
 * them, so glibc finds its PT_TLS (static TLS) and the rest. */
static void find_phdr(uint64_t *phdr, uint64_t *phdrsz)
{
    const uint8_t *img = (const uint8_t *)(uintptr_t)(MOD_VA + 0x200000ull);
    uint64_t phoff = get(img + 32, 8);
    uint64_t phnum = get(img + 56, 2);
    for (uint64_t i = 0; i < phnum; i++) {
        const uint8_t *ph = img + phoff + i * 56u;
        uint64_t off = get(ph + 8, 8), va = get(ph + 16, 8), filesz = get(ph + 32, 8);
        if (get(ph, 4) == 1u && off <= phoff && phoff + phnum * 56u <= off + filesz) {
            *phdr = va + (phoff - off);
            *phdrsz = phnum * 56u;
            return;
        }
    }
    die("the program's headers aren't in a loaded segment");
}

/* The task in `task` laid out to start; its stack pointer. */
static uint64_t lay_out(uint64_t task, uint64_t entry)
{
    /* The arrays: init ports (all null), init ints, an empty descriptor
     * table. */
    clear();
    uint64_t ports = GT_ARRAYS_VA, ints = GT_ARRAYS_VA + 64u, dtable = GT_ARRAYS_VA + 128u;
    put32(64u + 4u * INIT_UMASK, 022u);
    give_page(task, GT_ARRAYS_VA, J_MAP_W, page, 0, J_PAGE);

    for (unsigned i = 0; i < GT_STACK_PAGES; i++) {
        give_page(task, GT_STACK_LO + (uint64_t)i * J_PAGE, J_MAP_W, page, 0, 0);
    }

    clear();
    uint64_t s = 512u; /* the strings, after the words */
    uint64_t a0 = put_str(&s, "glibctest"), a1 = put_str(&s, "joist");
    uint64_t e0 = put_str(&s, "JOIST=phase5");
    uint64_t w = 0;
    put64(w, 2u); /* argc */
    put64(w += 8u, a0);
    put64(w += 8u, a1);
    put64(w += 8u, 0);
    put64(w += 8u, e0);
    put64(w += 8u, 0);
    uint64_t d = w + 8u;
    uint64_t phdr = 0, phdrsz = 0;
    find_phdr(&phdr, &phdrsz);
    uint64_t v[HSD_WORDS] = {0};
    v[HSD_DTABLE] = dtable;
    v[HSD_DTABLESIZE] = 0;
    v[HSD_PORTARRAY] = ports;
    v[HSD_PORTARRAYSIZE] = INIT_PORT_MAX;
    v[HSD_INTARRAY] = ints;
    v[HSD_INTARRAYSIZE] = INIT_INT_MAX;
    v[HSD_STACK_BASE] = GT_STACK_LO;
    v[HSD_STACK_SIZE] = GT_STACK_PAGES * (uint64_t)J_PAGE;
    v[HSD_PHDR] = phdr;
    v[HSD_PHDRSZ] = phdrsz;
    v[HSD_USER_ENTRY] = entry;
    for (unsigned i = 0; i < HSD_WORDS; i++) {
        put64(d + 8u * i, v[i]);
    }
    give_page(task, GT_BLOCK_VA, J_MAP_W, page, 0, J_PAGE);
    return GT_BLOCK_VA;
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 2) {
        die("needs two modules: mm, a glibc program");
    }
    test_init();
    start_mm();
    expect_mm(0, HOST_POOL_PAGES, "a fresh mm isn't empty");
    struct j_counters base;
    must(j_debug_counters(&base), "debug_counters");

    struct worker k;
    k.task = slot_new();
    k.thread = slot_new();
    k.sc = slot_new();
    uint64_t entry = ld_load_quota(1, k.task, GT_DONATION);
    uint64_t sp = lay_out(k.task, entry);
    must(j_cap_give(mm_task, k.task, MM_S_ROOT, J_R_ALL), "cap_give (the program, to mm)");
    struct j_msg r = ask(MM_REQ_ADOPT, MM_S_ROOT);
    check(r.r[0] == MM_OK, "mm's ADOPT failed");
    must(j_thread_spawn(k.task, k.thread, entry, sp), "thread_spawn (the program)");
    must(j_cap_give(k.task, k.task, 1, J_R_ALL), "cap_give (the task's own)");
    must(j_cap_give(k.task, k.thread, 2, J_R_ALL), "cap_give (its thread's)");
    j_puts("glibcroot: started\n");
    worker_run(&k, GT_PRIO, 10 * J_MS, 10 * J_MS);

    /* It ends itself; its thread then is gone. Looked at by reading its
     * registers, which leaves a wait it is in alone (a suspend would abort
     * it: E_ABORTED, glibc's EINTR). */
    uint64_t waited = 0;
    struct j_thread_regs regs;
    while (j_thread_read_regs(k.thread, &regs) != J_E_GONE) {
        check(waited < GT_WAIT_NS, "the program didn't end");
        sleep_ns(10u * J_MS);
        waited += 10u * J_MS;
    }
    j_puts("glibcroot: the program ended\n");
    (void)j_cap_delete(k.thread);
    (void)j_cap_delete(k.sc);
    (void)j_cap_delete(k.task);
    int more = 1;
    for (unsigned i = 0; more && i < 1000u; i++) {
        must(j_continue(1000, &more), "continue");
    }
    expect_mm(0, HOST_POOL_PAGES, "mm didn't forget the program, or lost pages");
    struct j_counters c;
    must(j_debug_counters(&c), "debug_counters");
    check(c.spaces == base.spaces && c.threads == base.threads, "the program left a space or a thread");
    j_puts("glibcroot: all ok\n");
    j_exit(0);
}
