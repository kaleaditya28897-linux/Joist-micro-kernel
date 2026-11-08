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
 * Its descriptors 0, 1 and 2 are a console the root serves (the owner's
 * choice, 2026-11-07, for glibc's own tests: item 11 d): send rights in
 * slots 5, 6 and 7 to the root's endpoint, whose io_write
 * (console.defs, joistmig's server, built with glibc's headers by
 * tools/phase5/glibc-test.sh and linked in as GLIBC_CONSOLE) puts the
 * bytes out through the kernel's debug write; any other message is
 * answered MIG_BAD_ID. Its bootstrap port (slot 3) is the same endpoint
 * with another badge, where glibc's _exit, with no proc server, sends the
 * exit status (glibc/sysdeps/joist/joist-special.c, JOIST_BOOT_EXIT).
 *
 * Module 2, if there is one, is the program's arguments and environment,
 * as text, a line a thing (put_args); without it, "glibctest joist" and
 * JOIST=phase5. Module 3, if there is one, is its stdin, which io_read
 * gives out from the start to its end (module 2 says how long it is),
 * then end of file, on any of the three descriptors.
 *
 * Then it serves the program until it ends itself (exit: task_terminate,
 * the task's own TASK_DESTROY), prints the exit status it was given, and
 * sees that mm forgot the task and has every page back. */
#define TEST_NAME "glibcroot"
#include "../lib/testroot.h"
/* mm's pool: at most one of the root's frames; the program's tree may have
 * half. A build for glibc's tests (tools/phase5/glibc-tests-run.sh) sets a
 * larger one, with larger frames, and more pages for mm's own tables. */
#ifndef GT_POOL_PAGES
#define GT_POOL_PAGES 512u
#endif
#define HOST_POOL_PAGES GT_POOL_PAGES
#ifdef GT_MM_TABLES
#define HOST_MM_TABLES GT_MM_TABLES
#endif
#include "../lib/mmhost.h"

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free);

/* The program's stack: STACK_TOP down, the start-up block in the page at
 * STACK_TOP (sp points at its first word), the arrays in a page below the
 * stack, a guard gap between. All below the image (CHILD_LO) and below
 * 4 GiB: glibc's anywhere window starts there (joist-vm.h). 128 KiB; a
 * build for glibc's tests (tools/phase5/glibc-tests-run.sh) sets 3 MiB,
 * the most there is room for below the image (some tests have megabytes
 * on the stack). */
#ifndef GT_STACK_PAGES
#define GT_STACK_PAGES 32u
#endif
_Static_assert(GT_STACK_PAGES >= 1u && GT_STACK_PAGES <= 768u, "the stack must fit below the image");
#define GT_BLOCK_VA    (CHILD_LO - 0x21000ull)                          /* 0x3df000 */
#define GT_STACK_LO    (GT_BLOCK_VA - GT_STACK_PAGES * (uint64_t)J_PAGE) /* 0x3bf000 */
#define GT_ARRAYS_VA   (GT_STACK_LO - 0x10000ull)                       /* 0x3af000 */
#define GT_DONATION    512u /* glibc's objects: a notification and two badges a thread, a
                             * context, 16 futex buckets, its name table's leaves */
#define GT_PRIO        100u
#define GT_WAIT_NS     (60u * 1000u * J_MS)
/* The program's slots for its console and bootstrap port (below glibc's
 * fixed names: joist-ports.h), and the badges on the root's endpoint. */
#define GT_SLOT_BOOT   3u
#define GT_SLOT_FD0    5u
#define GT_FDS         3u
#define GT_BADGE_CON   1u
#define GT_BADGE_BOOT  2u
#define GT_BOOT_EXIT   0x4a4f0001u /* joist-ports.h's JOIST_BOOT_EXIT */
#define GT_ARGS_VA     (MOD_VA + 2u * 0x200000ull)
#define GT_ARGS_MAX    3000u      /* bytes of strings: they share the block's page */
#define GT_ARGS_WORDS  56u        /* argv and envp pointers, at most */

/* Hurd's numbers (hurd/hurd_types.h). */
#define INIT_PORT_MAX 6u
#define INIT_INT_MAX  5u
#define INIT_UMASK    0u
#define INIT_PORT_BOOTSTRAP 5u

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

static uint64_t stdin_len; /* bytes of module 3, the program's stdin */

/* The program's argv and environment: module 2's text, put in the block
 * from `s` on; their addresses in v (argv, a null pointer, envp, a null
 * pointer); argc. A line a thing: "a ARG" an argument, "e NAME=VALUE" an
 * environment entry, "i BYTES" the length of the stdin in module 3. */
static uint64_t put_args(uint64_t nmods, uint64_t *s, uint64_t *v)
{
    if (nmods < 3) {
        v[0] = put_str(s, "glibctest");
        v[1] = put_str(s, "joist");
        v[2] = 0;
        v[3] = put_str(s, "JOIST=phase5");
        v[4] = 0;
        return 2;
    }
    uint64_t pages = j_frame_pages(ld_mod_first + 2u);
    if (pages == 0 || pages > 1u) {
        die("module 2, the arguments, isn't one page");
    }
    must(j_map_run(ld_mod_first + 2u, 0, 1, J_ROOT_SELF, GT_ARGS_VA / J_PAGE, 0), "map (arguments)");
    const char *t = (const char *)(uintptr_t)GT_ARGS_VA;
    uint64_t env[GT_ARGS_WORDS];
    uint64_t argc = 0, envc = 0;
    for (uint64_t i = 0; i < J_PAGE && t[i] != '\0';) {
        char kind = t[i];
        uint64_t j = i + 2u; /* past the kind and its space */
        uint64_t e = i;
        while (e < J_PAGE && t[e] != '\0' && t[e] != '\n') {
            e++;
        }
        if (j > e || (kind != 'a' && kind != 'e' && kind != 'i')) {
            die("module 2: a line isn't 'a', 'e' or 'i' and a space");
        }
        if (kind == 'i') {
            stdin_len = 0;
            for (; j < e && t[j] >= '0' && t[j] <= '9'; j++) {
                stdin_len = stdin_len * 10u + (uint64_t)(t[j] - '0');
            }
        } else {
            if (argc + envc + 3u >= GT_ARGS_WORDS || *s + (e - j) + 1u > GT_ARGS_MAX + 512u) {
                die("module 2's arguments are too many or too long");
            }
            uint64_t at = GT_BLOCK_VA + *s;
            for (; j < e; j++) {
                page[(*s)++] = (uint8_t)t[j];
            }
            page[(*s)++] = 0;
            if (kind == 'a') {
                v[argc++] = at;
            } else {
                env[envc++] = at;
            }
        }
        i = e < J_PAGE && t[e] == '\n' ? e + 1u : e;
    }
    uint64_t n = argc;
    v[n++] = 0;
    for (uint64_t k = 0; k < envc; k++) {
        v[n++] = env[k];
    }
    v[n] = 0;
    return argc;
}

/* The task in `task` laid out to start; its stack pointer. */
static uint64_t lay_out(uint64_t task, uint64_t entry, uint64_t nmods)
{
    /* The arrays: init ports (all null but the bootstrap port), init ints,
     * the descriptor table
     * (the console, three times). */
    clear();
    uint64_t ports = GT_ARRAYS_VA, ints = GT_ARRAYS_VA + 64u, dtable = GT_ARRAYS_VA + 128u;
    put32(64u + 4u * INIT_UMASK, 022u);
    /* The bootstrap port: _hurd_init makes it the task's from here. */
    put32(4u * INIT_PORT_BOOTSTRAP, GT_SLOT_BOOT);
    for (unsigned i = 0; i < GT_FDS; i++) {
        put32(128u + 4u * i, GT_SLOT_FD0 + i);
    }
    give_page(task, GT_ARRAYS_VA, J_MAP_W, page, 0, J_PAGE);

    for (unsigned i = 0; i < GT_STACK_PAGES; i++) {
        give_page(task, GT_STACK_LO + (uint64_t)i * J_PAGE, J_MAP_W, page, 0, 0);
    }

    clear();
    uint64_t s = 512u; /* the strings, after the words */
    uint64_t av[GT_ARGS_WORDS];
    uint64_t argc = put_args(nmods, &s, av);
    uint64_t w = 0;
    put64(w, argc);
    unsigned nulls = 0;
    for (unsigned i = 0; nulls < 2u; i++) {
        put64(w += 8u, av[i]);
        nulls += av[i] == 0;
    }
    uint64_t d = w + 8u;
    uint64_t phdr = 0, phdrsz = 0;
    find_phdr(&phdr, &phdrsz);
    uint64_t v[HSD_WORDS] = {0};
    v[HSD_DTABLE] = dtable;
    v[HSD_DTABLESIZE] = GT_FDS;
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

/* ---- the console and the bootstrap port ---- */

/* MIG's runtime, as console.defs' server sees it (user/lib/joist_mig.h);
 * the server itself is GLIBC_CONSOLE's, weak: a root built without glibc
 * answers every message MIG_BAD_ID. */
#include "../lib/joist_mig.h"
int io_server(struct joist_mig_msg *in, struct joist_mig_msg *out) __attribute__((weak));
/* The routine it calls: Hurd's types (kern_return_t, io_t, data_t,
 * mach_msg_type_number_t, loff_t, vm_size_t) in their sizes. */
int io_write(uint32_t io, char *data, uint32_t n, int64_t offset, uint64_t *amount);

int io_write(uint32_t io, char *data, uint32_t n, int64_t offset, uint64_t *amount)
{
    (void)offset;
    if (io != GT_BADGE_CON) {
        return 1100; /* EOPNOTSUPP's Hurd value is unknown here: any error */
    }
    (void)j_debug_write(data, n);
    *amount = n;
    return 0;
}

/* The stdin: module 3, mapped here; the next byte io_read gives. */
#define GT_STDIN_VA (MOD_VA + 3u * 0x200000ull)
static const uint8_t *stdin_bytes;
static uint64_t stdin_at;

int io_read(uint32_t io, char **data, uint32_t *n, int64_t offset, uint64_t amount);

int io_read(uint32_t io, char **data, uint32_t *n, int64_t offset, uint64_t amount)
{
    (void)offset;
    if (io != GT_BADGE_CON) {
        return 1100;
    }
    uint64_t k = stdin_bytes == 0 || stdin_at >= stdin_len ? 0 : stdin_len - stdin_at;
    k = k < amount ? k : amount;
    k = k < *n ? k : *n; /* the reply's in-line room, which the server gives */
    for (uint64_t i = 0; i < k; i++) {
        (*data)[i] = (char)stdin_bytes[stdin_at + i];
    }
    stdin_at += k;
    *n = (uint32_t)k;
    return 0;
}

/* Module 3, if there is one: the stdin. */
static void map_stdin(uint64_t nmods)
{
    if (nmods < 4) {
        return;
    }
    uint64_t pages = j_frame_pages(ld_mod_first + 3u);
    if (pages == 0 || pages > J_MAP_SPAN || stdin_len > pages * J_PAGE) {
        die("module 3, the stdin, is empty, over 2 MiB, or shorter than module 2 says");
    }
    must(j_map_run(ld_mod_first + 3u, 0, pages, J_ROOT_SELF, GT_STDIN_VA / J_PAGE, 0), "map (stdin)");
    stdin_bytes = (const uint8_t *)(uintptr_t)GT_STDIN_VA;
}

static uint64_t root_ep_c;
static __attribute__((aligned(J_PAGE))) uint64_t ipc_buf[J_MSG_WORDS];
static uint64_t out_buf[J_MSG_WORDS];
static int exited;
static uint64_t exit_status;

/* One message, if one comes within ns: answered. */
static void serve_one(uint64_t ns)
{
    struct j_msg m = {0};
    if (j_recv(root_ep_c, &m, ns) != J_OK) {
        return;
    }
    struct j_msg rep = {0};
    if (m.badge == GT_BADGE_BOOT && (uint32_t)m.r[0] == GT_BOOT_EXIT) {
        exited = 1;
        exit_status = m.r[1];
    } else if (m.badge == GT_BADGE_CON && io_server != 0) {
        struct joist_mig_msg in = {0};
        struct joist_mig_msg out = {0};
        for (unsigned i = 0; i < 4u; i++) {
            in.r[i] = m.r[i];
        }
        in.len = J_WORDS(m.caps);
        in.buf = ipc_buf;
        in.badge = m.badge;
        out.buf = out_buf;
        if (!io_server(&in, &out)) {
            out.r[0] = (uint32_t)JOIST_MIG_BAD_ID;
            out.len = 0;
        }
        for (unsigned i = 0; i < 4u; i++) {
            rep.r[i] = out.r[i];
        }
        for (uint32_t i = 0; i < out.len && i < J_MSG_WORDS; i++) {
            ipc_buf[i] = out_buf[i];
        }
        rep.caps = J_WORDS_SET(out.len);
    } else {
        rep.r[0] = (uint32_t)JOIST_MIG_BAD_ID;
    }
    (void)j_reply(&rep);
}

/* What the root's console needs of MIG's runtime: nothing calls them. */
int joist_mig_call(uint32_t port, struct joist_mig_msg *m)
{
    (void)port;
    (void)m;
    return 1;
}
void *joist_mig_allocate(size_t bytes)
{
    (void)bytes;
    return 0;
}
void joist_mig_deallocate(void *p, size_t bytes)
{
    (void)p;
    (void)bytes;
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
    uint64_t sp = lay_out(k.task, entry, nmods);
    map_stdin(nmods);
    root_ep_c = slot_new();
    must(j_obj_create(J_ENDPOINT, root_ep_c), "obj_create (console)");
    must(j_ipc_buffer(ipc_buf), "ipc_buffer");
    uint64_t tmp = slot_new();
    must(j_cap_mint(root_ep_c, tmp, J_R_SEND | J_R_GRANT, GT_BADGE_CON), "cap_mint (console)");
    for (unsigned i = 0; i < GT_FDS; i++) {
        must(j_cap_give(k.task, tmp, GT_SLOT_FD0 + i, J_R_SEND), "cap_give (console)");
    }
    must(j_cap_delete(tmp), "cap_delete (console)");
    must(j_cap_mint(root_ep_c, tmp, J_R_SEND | J_R_GRANT, GT_BADGE_BOOT), "cap_mint (bootstrap)");
    must(j_cap_give(k.task, tmp, GT_SLOT_BOOT, J_R_SEND), "cap_give (bootstrap)");
    must(j_cap_delete(tmp), "cap_delete (bootstrap)");
    must(j_cap_give(mm_task, k.task, MM_S_ROOT, J_R_ALL), "cap_give (the program, to mm)");
    struct j_msg r = ask(MM_REQ_ADOPT, MM_S_ROOT);
    check(r.r[0] == MM_OK, "mm's ADOPT failed");
    must(j_thread_spawn(k.task, k.thread, entry, sp), "thread_spawn (the program)");
    must(j_cap_give(k.task, k.task, 1, J_R_ALL), "cap_give (the task's own)");
    must(j_cap_give(k.task, k.thread, 2, J_R_ALL), "cap_give (its thread's)");
    j_puts("glibcroot: started\n");
    worker_run(&k, GT_PRIO, 10 * J_MS, 10 * J_MS);

    /* It ends itself (exit: its task destroyed, its threads with it), or
     * its last thread dies (a fault mm refused). Not its first thread's
     * end: that one may pthread_exit while others go on. Watched by the
     * kernel's count of live threads, above base's while one of the
     * program's is; its first thread's registers read for a TIMEOUT
     * (which leaves a wait it is in alone: a suspend would abort it,
     * E_ABORTED, glibc's EINTR). */
    uint64_t t0, now, ncpu;
    must(j_clock(&t0, &ncpu), "clock");
    struct j_thread_regs regs;
    struct j_counters live;
    for (;;) {
        must(j_debug_counters(&live), "debug_counters");
        if (live.threads <= base.threads) {
            break;
        }
        must(j_clock(&now, &ncpu), "clock");
        if (now - t0 >= GT_WAIT_NS) {
            /* Where its first thread is, for llvm-addr2line. */
            if (j_thread_read_regs(k.thread, &regs) == J_OK) {
                j_puts("glibcroot: TIMEOUT, its first thread at pc ");
                j_putu(regs.pc);
                j_puts(", sp ");
                j_putu(regs.sp);
                j_puts("\n");
            } else {
                j_puts("glibcroot: TIMEOUT, its first thread gone\n");
            }
            j_exit(1);
        }
        serve_one(10u * J_MS); /* a message, or 10 ms */
    }
    j_puts("glibcroot: the program ended\n");
    if (exited) {
        /* _hurd_exit's: W_EXITCODE (code, 0), or a signal's number. */
        j_puts("glibcroot: exit status ");
        j_putu(exit_status);
        j_puts(", code ");
        j_putu((exit_status >> 8) & 0xffu);
        j_puts("\n");
    } else {
        j_puts("glibcroot: no exit status\n");
    }
    /* Its threads gone without exit: the task is still there (mm holds a
     * capability to it), so it is destroyed here, as exit would have. */
    (void)j_task_destroy(k.task);
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
