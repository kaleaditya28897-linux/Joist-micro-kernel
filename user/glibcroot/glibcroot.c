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
 * exit status (glibc/sysdeps/joist/joist-special.c, JOIST_BOOT_EXIT), and
 * from where it asks for its pid at start-up (JOIST_BOOT_PID).
 *
 * Module 2, if there is one, is the program's arguments and environment,
 * as text, a line a thing (put_args); without it, "glibctest joist" and
 * JOIST=phase5. Module 3, if there is one, is its stdin, which io_read
 * gives out from the start to its end (module 2 says how long it is),
 * then end of file, on any of the three descriptors: as much as a read
 * asks for, past the message through the caller's write window (0032).
 *
 * Then it serves the program until it ends itself (exit: task_terminate,
 * the task's own TASK_DESTROY), prints the exit status it was given, and
 * sees that mm forgot the task and has every page back.
 *
 * Built with GT_HELLO (Phase 5 step 10; the owner's design of 2026-11-16),
 * the program is the Hurd's trans/hello (tools/phase5/hello.sh), and the
 * root is the filesystem that attaches it and its first client: its
 * bootstrap port also answers fsys_startup (rootfs.defs: the control port
 * kept), then the root takes hello's root node with fsys_getroot and calls
 * it through joistmig's user stubs (io_read, io_seek, io_stat, io_write
 * refused), the stubs and the server built with glibc's headers into
 * GLIBC_HELLO. Last, hello is killed while a call of the root's waits in
 * it: the call fails, and the root's teardown checks finds nothing left. */
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
#define GT_BADGE_NODE  4u /* GT_HELLO: the node hello is attached to */
#define GT_BOOT_EXIT   0x4a4f0001u /* joist-ports.h's JOIST_BOOT_EXIT */
#define GT_BOOT_PID    0x4a4f0002u /* and JOIST_BOOT_PID */
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
#ifdef GT_HELLO
        v[0] = put_str(s, "hello"); /* no arguments: it takes none to start */
        v[1] = 0;
        v[2] = 0;
        return 1;
#else
        v[0] = put_str(s, "glibctest");
        v[1] = put_str(s, "joist");
        v[2] = 0;
        v[3] = put_str(s, "JOIST=phase5");
        v[4] = 0;
        return 2;
#endif
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
    if (io != GT_BADGE_CON) {
        return 1100;
    }
    /* At OFFSET, or (-1) at the file pointer, which only then moves: the
     * Hurd's io_read (pread, Phase 5 step 10's measurement). */
    uint64_t at = offset >= 0 ? (uint64_t)offset : stdin_at;
    uint64_t k = stdin_bytes == 0 || at >= stdin_len ? 0 : stdin_len - at;
    k = k < amount ? k : amount;
    if (k > *n) {
        /* Past the room the server stub gives (its in-line buffer): the
         * module's own bytes, which the stub copies into the slice of the
         * caller's write window offered for them (decision 0032: a read
         * past the message), or refuses (MIG_ARRAY_TOO_LARGE) if it
         * offered none. The stub "frees" them after (dealloc):
         * joist_mig_deallocate is nothing here. */
        *data = (char *)(uintptr_t)(stdin_bytes + at);
    } else {
        memcpy(*data, stdin_bytes + at, (size_t)k);
    }
    if (offset < 0) {
        stdin_at += k;
    }
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
static uint64_t next_pid = 2; /* the pids the root gives (1 would be init's) */
static int exited;
static uint64_t exit_status;

#ifdef GT_HELLO
/* Where capabilities coming to the root land (a receive and a call's
 * reply alike): two slots, a fresh one once one is taken. */
static uint64_t land[2];
static void landing(void)
{
    for (unsigned i = 0; i < 2u; i++) {
        if (land[i] == 0) {
            land[i] = slot_new();
        }
    }
    must(j_recv_slots(land[0], land[1], 2), "recv_slots");
}
/* The capabilities a message brought, in its slots: no longer free. */
static void landed(uint64_t caps)
{
    for (unsigned i = 0; i < J_CAPS_COUNT(caps); i++) {
        uint64_t at = J_CAPS_SLOT(caps, i);
        for (unsigned k = 0; k < 2u; k++) {
            if (land[k] == at) {
                land[k] = 0;
            }
        }
    }
}
int fsys_server(struct joist_mig_msg *in, struct joist_mig_msg *out) __attribute__((weak));
#define HELLO_FSYS_STARTUP 22000u /* fsys.defs' first routine */
/* The node's servers (rootnode.defs, rootfile.defs: io and fs, prefixed). */
int node_io_server(struct joist_mig_msg *in, struct joist_mig_msg *out) __attribute__((weak));
int node_fs_server(struct joist_mig_msg *in, struct joist_mig_msg *out) __attribute__((weak));
static int node_server(struct joist_mig_msg *in, struct joist_mig_msg *out)
{
    return (node_io_server != 0 && node_io_server(in, out)) ||
           (node_fs_server != 0 && node_fs_server(in, out));
}
#endif

/* One message, if one comes within ns: answered. */
static void serve_one(uint64_t ns)
{
    struct j_msg m = {0};
#ifdef GT_HELLO
    landing();
#endif
    if (j_recv(root_ep_c, &m, ns) != J_OK) {
        return;
    }
#ifdef GT_HELLO
    landed(m.caps);
#endif
    struct j_msg rep = {0};
    if (m.badge == GT_BADGE_BOOT && (uint32_t)m.r[0] == GT_BOOT_PID) {
        /* The program's pid (no proc server yet): one word of body. */
        rep.r[0] = (uint64_t)8u << 32;
        rep.r[1] = next_pid++;
    } else if (m.badge == GT_BADGE_BOOT && (uint32_t)m.r[0] == GT_BOOT_EXIT) {
        exited = 1;
        exit_status = m.r[1];
    } else if ((m.badge == GT_BADGE_CON && io_server != 0)
#ifdef GT_HELLO
               || (m.badge == GT_BADGE_BOOT && fsys_server != 0 &&
                   (uint32_t)m.r[0] == HELLO_FSYS_STARTUP)
               || m.badge == GT_BADGE_NODE
#endif
    ) {
        struct joist_mig_msg in = {0};
        struct joist_mig_msg out = {0};
        for (unsigned i = 0; i < 4u; i++) {
            in.r[i] = m.r[i];
        }
        in.len = J_WORDS(m.caps);
        in.buf = ipc_buf;
        in.badge = m.badge;
        in.ncaps = J_CAPS_COUNT(m.caps);
        for (unsigned i = 0; i < in.ncaps; i++) {
            in.cap[i] = J_CAPS_SLOT(m.caps, i);
        }
        out.buf = out_buf;
        int (*server)(struct joist_mig_msg *, struct joist_mig_msg *) = io_server;
#ifdef GT_HELLO
        if (m.badge == GT_BADGE_BOOT) {
            server = fsys_server;
        } else if (m.badge == GT_BADGE_NODE) {
            server = node_server;
        }
#endif
#ifdef GT_HELLO
        (void)joist_mig_nulls_in(&in); /* null ports back in their places */
#endif
        if (!server(&in, &out)) {
            out.r[0] = (uint32_t)JOIST_MIG_BAD_ID;
            out.len = 0;
            out.ncaps = 0;
        }
#ifdef GT_HELLO
        /* The answer's rights (fsys_startup's realnode, null: marked). */
        joist_mig_nulls_out(&out);
        uint32_t nout = out.ncaps <= 2u ? out.ncaps : 0;
        rep.caps = nout;
        for (uint32_t i = 0; i < nout; i++) {
            uint64_t mask = out.mask[i] != 0 ? out.mask[i] : J_R_SEND;
            rep.caps |= (uint64_t)out.cap[i] << (2u + 20u * i) | mask << (18u + 20u * i);
        }
#endif
        for (unsigned i = 0; i < 4u; i++) {
            rep.r[i] = out.r[i];
        }
        for (uint32_t i = 0; i < out.len && i < J_MSG_WORDS; i++) {
            ipc_buf[i] = out_buf[i];
        }
        rep.caps |= J_WORDS_SET(out.len);
    } else {
        rep.r[0] = (uint32_t)JOIST_MIG_BAD_ID;
    }
    (void)j_reply(&rep);
}

#ifdef GT_HELLO
/* The client thread's own: its IPC buffer, and slots for capabilities its
 * calls' replies bring, taken from a pool the main thread made (the
 * loader's slot_new is the main thread's alone). */
static __attribute__((aligned(J_PAGE))) uint64_t cbuf[J_MSG_WORDS];
#define CLIENT_SLOTS 32u
static uint64_t cslot[CLIENT_SLOTS];
static unsigned cslot_next;
static uint64_t cland[2];
static void client_landing(void)
{
    for (unsigned i = 0; i < 2u; i++) {
        if (cland[i] == 0) {
            if (cslot_next == CLIENT_SLOTS) {
                die("hello: the client thread ran out of slots");
            }
            cland[i] = cslot[cslot_next++];
        }
    }
    must(j_recv_slots(cland[0], cland[1], 2), "recv_slots (client)");
}

/* A Call on the capability in slot `port`, for the user stubs (from the
 * client thread): the message's words through its IPC buffer, its
 * capabilities (rights as the stub asks, deleted after if it moves them),
 * the reply's landing in fresh slots. The kernel's refusals as Mach's
 * MACH_SEND_INVALID_DEST (the server gone) or MACH_SEND_INTERRUPTED (any
 * other), never 0. */
#define HELLO_CALL_NS (10u * 1000u * J_MS)
int joist_mig_call(uint32_t port, struct joist_mig_msg *m)
{
    struct j_msg c = {0};
    for (unsigned i = 0; i < 4u; i++) {
        c.r[i] = m->r[i];
    }
    joist_mig_nulls_out(m); /* null ports marked, not sent */
    for (unsigned i = 0; i < 4u; i++) {
        c.r[i] = m->r[i];
    }
    uint32_t len = m->len <= J_MSG_WORDS ? m->len : 0;
    for (uint32_t i = 0; i < len; i++) {
        cbuf[i] = m->buf[i];
    }
    uint32_t n = m->ncaps <= 2u ? m->ncaps : 0;
    c.caps = J_WORDS_SET(len) | n;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t mask = m->mask[i] != 0 ? m->mask[i] : J_R_SEND;
        c.caps |= (uint64_t)m->cap[i] << (2u + 20u * i) | mask << (18u + 20u * i);
    }
    client_landing();
    uint64_t e = j_call(port, &c, HELLO_CALL_NS);
    if (e != J_OK && e != J_E_PEER && e != J_E_GONE) {
        j_puts("hello: the kernel refused a call: ");
        j_putu(e);
        j_puts("\n");
    }
    if (e != J_OK) {
        return e == J_E_PEER || e == J_E_GONE ? 0x10000003 : 0x10000007;
    }
    for (unsigned i = 0; i < J_CAPS_COUNT(c.caps); i++) {
        for (unsigned k = 0; k < 2u; k++) {
            if (cland[k] == J_CAPS_SLOT(c.caps, i)) {
                cland[k] = 0;
            }
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        if (m->move[i]) {
            (void)j_cap_delete(m->cap[i]);
        }
    }
    for (unsigned i = 0; i < 4u; i++) {
        m->r[i] = c.r[i];
    }
    m->len = J_WORDS(c.caps);
    for (uint32_t i = 0; i < m->len && i < J_MSG_WORDS; i++) {
        m->buf[i] = cbuf[i];
    }
    m->ncaps = J_CAPS_COUNT(c.caps);
    for (uint32_t i = 0; i < m->ncaps; i++) {
        m->cap[i] = J_CAPS_SLOT(c.caps, i);
    }
    (void)joist_mig_nulls_in(m); /* the reply's null ports back */
    return 0;
}
#else
/* What the root's console needs of MIG's runtime: nothing calls them. */
int joist_mig_call(uint32_t port, struct joist_mig_msg *m)
{
    (void)port;
    (void)m;
    return 1;
}
#endif
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
/* Decision 0032's copies, through the reply serve_one holds while io_server
 * runs; the client's windows are not this root's (it makes no calls). */
int joist_mig_reply_read(uint64_t off, void *dst, size_t bytes, size_t *done)
{
    uint64_t n = 0;
    uint64_t e = j_reply_read(J_SLOT_NONE, off, dst, bytes, &n);
    *done = e == J_OK ? (size_t)n : 0;
    return e == J_OK ? 0 : -1;
}
int joist_mig_reply_write(uint64_t off, const void *src, size_t bytes, size_t *done)
{
    uint64_t n = 0;
    uint64_t e = j_reply_write(J_SLOT_NONE, off, src, bytes, &n);
    *done = e == J_OK ? (size_t)n : 0;
    return e == J_OK ? 0 : -1;
}
int joist_mig_window(unsigned kind, const void *va, size_t bytes)
{
    (void)kind;
    (void)va;
    (void)bytes;
    return -1;
}
void *joist_mig_window_allocate(size_t bytes)
{
    (void)bytes;
    return 0;
}
void joist_mig_window_free(void *p, size_t bytes)
{
    (void)p;
    (void)bytes;
}

#ifdef GT_HELLO
/* ---- the filesystem that attaches hello, and its first client ---- */

/* The Hurd's numbers (hurd/hurd_types.h, fcntl.h, errno.h on the Hurd). */
#define HURD_O_READ          1u
#define HURD_SEEK_SET        0
#define HURD_SEEK_CUR        1
#define HURD_SEEK_END        2
#define HURD_EBADF           0x40000009
#define MACH_COPY_SEND       19u /* MACH_MSG_TYPE_COPY_SEND */
#define HURD_FS_RETRY_NORMAL 1   /* retry_type: this is the file (retry_name empty) */
#define HELLO_TEXT           "Hello, world!\n"
#define HELLO_LEN            14u

/* hello's control port, from its fsys_startup (0: not yet). */
static uint64_t hello_control;
/* The node it is attached to: a send right to the root's endpoint, badged
 * GT_BADGE_NODE, given to it as fsys_startup's realnode and again whenever
 * it asks for another (io_duplicate, io_restrict_auth). */
static uint64_t node_cap;
#define NODE_SIZE 0 /* hello reports its own contents' size */

/* rootfs.defs' routine (joistmig's server, GLIBC_HELLO): the translator's
 * control port kept, no underlying node answered. */
int S_fsys_startup(uint32_t bootstrap, int openflags, uint32_t control_port, uint32_t *realnode,
                   uint32_t *realnodePoly);
int S_fsys_startup(uint32_t bootstrap, int openflags, uint32_t control_port, uint32_t *realnode,
                   uint32_t *realnodePoly)
{
    (void)bootstrap;
    (void)openflags;
    if (hello_control != 0 || control_port == 0) {
        return 0x40000011; /* EEXIST, or EINVAL: one translator here */
    }
    hello_control = control_port;
    *realnode = (uint32_t)node_cap;
    *realnodePoly = MACH_COPY_SEND;
    return 0;
}

/* The node's routines (rootnode.defs, rootfile.defs): what libtrivfs asks
 * of the node it is attached to. */
void hr_stat_fill(void *st, long long size);
int node_S_io_stat(uint32_t io, void *st);
int node_S_io_stat(uint32_t io, void *st)
{
    (void)io;
    hr_stat_fill(st, NODE_SIZE);
    return 0;
}
int node_S_io_restrict_auth(uint32_t io, uint32_t *new_object, uint32_t *poly, const uint32_t *uids,
                            uint32_t nuids, const uint32_t *gids, uint32_t ngids);
int node_S_io_restrict_auth(uint32_t io, uint32_t *new_object, uint32_t *poly, const uint32_t *uids,
                            uint32_t nuids, const uint32_t *gids, uint32_t ngids)
{
    (void)io;
    (void)uids;
    (void)nuids;
    (void)gids;
    (void)ngids;
    *new_object = (uint32_t)node_cap;
    *poly = MACH_COPY_SEND;
    return 0;
}
int node_S_io_duplicate(uint32_t io, uint32_t *newport, uint32_t *poly);
int node_S_io_duplicate(uint32_t io, uint32_t *newport, uint32_t *poly)
{
    (void)io;
    *newport = (uint32_t)node_cap;
    *poly = MACH_COPY_SEND;
    return 0;
}
int node_S_file_check_access(uint32_t file, int *allowed);
int node_S_file_check_access(uint32_t file, int *allowed)
{
    (void)file;
    *allowed = (int)HURD_O_READ;
    return 0;
}

/* The user stubs (joistmig's, prefixed hr_, built with glibc's types:
 * ports are names, loff_t 64 bits, data_t a char pointer). */
int hr_fsys_getroot(uint32_t fsys, uint32_t dotdot, uint32_t dotdotPoly, const uint32_t *uids,
                    uint32_t nuids, const uint32_t *gids, uint32_t ngids, int flags, int *do_retry,
                    char *retry_name, uint32_t *file);
int hr_io_read(uint32_t io, char **data, uint32_t *n, int64_t offset, uint64_t amount);
int hr_io_seek(uint32_t io, int64_t offset, int whence, int64_t *newp);
int hr_io_write(uint32_t io, const char *data, uint32_t n, int64_t offset, uint64_t *amount);
int hr_io_stat(uint32_t io, void *st);
unsigned long hr_stat_bytes(void);
long long hr_stat_size(const void *st);
unsigned int hr_stat_mode(const void *st);

static uint64_t rdtsc_now(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}

static char rbuf[4096];
static uint8_t statbuf[512];

/* io_read of `amount` at `offset` (-1: the file pointer): the bytes, as a
 * string's length, or -1 if refused. */
static int64_t hello_read(uint32_t file, int64_t offset, uint64_t amount, const char **got)
{
    char *data = rbuf;
    uint32_t n = sizeof rbuf;
    if (hr_io_read(file, &data, &n, offset, amount) != 0) {
        return -1;
    }
    *got = data;
    return n;
}

static int same(const char *a, const char *b, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* hello's first thread, for where it is when something fails. */
static uint64_t hello_thread;
static void hello_where(void)
{
    struct j_thread_regs regs;
    if (j_thread_read_regs(hello_thread, &regs) == J_OK) {
        j_puts("hello: its first thread at pc ");
        j_putu(regs.pc);
        j_puts(", sp ");
        j_putu(regs.sp);
        j_puts("\n");
    } else {
        j_puts("hello: its first thread is gone\n");
    }
}

/* The client's checks, on hello's root node. */
static uint32_t hello_client(void)
{
    uint32_t uids[1] = {0};
    uint32_t gids[1] = {0};
    int retry = -1;
    static char retry_name[1024];
    uint32_t file = 0;
    /* No parent directory (dotdot null): it is the root of nothing else. */
    int e = hr_fsys_getroot((uint32_t)hello_control, 0, MACH_COPY_SEND, uids, 1, gids, 1,
                            (int)HURD_O_READ, &retry, retry_name, &file);
    if (e != 0 || file == 0 || retry != HURD_FS_RETRY_NORMAL || retry_name[0] != 0) {
        j_puts("hello: fsys_getroot: result ");
        j_putu((uint64_t)(uint32_t)e);
        j_puts(", retry ");
        j_putu((uint64_t)(uint32_t)retry);
        j_puts(", file ");
        j_putu(file);
        j_puts("\n");
        hello_where();
    }
    check(e == 0 && file != 0 && retry == HURD_FS_RETRY_NORMAL && retry_name[0] == 0,
          "hello: fsys_getroot didn't give its root node");
    j_puts("hello: fsys_getroot ok\n");

    const char *got = 0;
    int64_t n = hello_read(file, 0, 100, &got);
    check(n == HELLO_LEN && same(got, HELLO_TEXT, HELLO_LEN), "hello: io_read of it whole");
    n = hello_read(file, 7, 5, &got);
    check(n == 5 && same(got, "world", 5), "hello: io_read at an offset");
    n = hello_read(file, HELLO_LEN, 10, &got);
    check(n == 0, "hello: io_read past the end isn't empty");
    j_puts("hello: io_read ok\n");

    /* The file pointer: io_read at -1 reads from it and moves it. */
    int64_t at = -1;
    check(hr_io_seek(file, 0, HURD_SEEK_SET, &at) == 0 && at == 0, "hello: io_seek SEEK_SET");
    n = hello_read(file, -1, 5, &got);
    check(n == 5 && same(got, "Hello", 5), "hello: io_read at the pointer");
    check(hr_io_seek(file, 2, HURD_SEEK_CUR, &at) == 0 && at == 7, "hello: io_seek SEEK_CUR");
    n = hello_read(file, -1, 100, &got);
    check(n == 7 && same(got, "world!\n", 7), "hello: io_read the rest");
    check(hr_io_seek(file, -1, HURD_SEEK_END, &at) == 0 && at == HELLO_LEN - 1u, "hello: io_seek SEEK_END");
    j_puts("hello: io_seek ok\n");

    check(hr_stat_bytes() <= sizeof statbuf, "hello: struct stat larger than the root's buffer");
    check(hr_io_stat(file, statbuf) == 0 && hr_stat_size(statbuf) == HELLO_LEN &&
              (hr_stat_mode(statbuf) & 0170000u) != 0,
          "hello: io_stat");
    j_puts("hello: io_stat ok\n");

    uint64_t wrote = 77;
    check(hr_io_write(file, "x", 1, 0, &wrote) == HURD_EBADF && wrote == 77, "hello: io_write not refused");
    j_puts("hello: io_write refused ok\n");

    /* The plan's measurement, a MIG round trip against the raw IPC one
     * (ipcbench): io_seek(SEEK_CUR, 0), which changes nothing, 2 000 times
     * after 200 to warm, each timed with the TSC: joistmig's user stub, the
     * Call, libports' loop, libtrivfs's demuxer and hello's routine, and
     * back. The median, the least and the greatest, in TSC counts. */
    static uint64_t rt[2000];
    for (unsigned i = 0; i < 2200u; i++) {
        uint64_t t0 = rdtsc_now();
        int r = hr_io_seek(file, 0, HURD_SEEK_CUR, &at);
        uint64_t d = rdtsc_now() - t0;
        check(r == 0, "hello: io_seek in the measurement");
        if (i >= 200u) {
            rt[i - 200u] = d;
        }
    }
    for (unsigned i = 1; i < 2000u; i++) { /* insertion sort: 2 000 values */
        uint64_t x = rt[i];
        unsigned k = i;
        for (; k > 0 && rt[k - 1u] > x; k--) {
            rt[k] = rt[k - 1u];
        }
        rt[k] = x;
    }
    j_puts("hello: a MIG round trip (io_seek): median ");
    j_putu(rt[1000]);
    j_puts(", least ");
    j_putu(rt[0]);
    j_puts(", most ");
    j_putu(rt[1999]);
    j_puts(" TSC counts\n");
    return file;
}

/* ---- the client thread, and hello killed while its call waits ---- */

/* Where the client thread is: 0 running its checks, 1 done with them (the
 * main thread then stops hello's serving thread), 2 told to make its last
 * call (into hello's queue), 3 that call ended (its result in last_e). */
static volatile int phase;
static volatile int64_t last_n;
static __attribute__((aligned(16))) uint8_t client_stack[65536];

static void client_thread(void)
{
    must(j_ipc_buffer(cbuf), "ipc_buffer (client)");
    uint32_t file = hello_client();
    phase = 1;
    while (phase != 2) {
        sleep_ns(J_MS);
    }
    const char *got = 0;
    last_n = hello_read(file, 0, 100, &got);
    phase = 3;
    for (;;) {
        sleep_ns(1000u * J_MS);
    }
}

/* The main thread: hello's messages served while the client checks it;
 * then its serving thread stopped, the client's last call made into its
 * queue, and its task destroyed under it: that call must fail. */
static void hello_run(uint64_t task, uint64_t thread)
{
    for (unsigned i = 0; i < CLIENT_SLOTS; i++) {
        cslot[i] = slot_new();
    }
    uint64_t ct = slot_new();
    uint64_t csc = slot_new();
    must(j_thread_spawn(J_ROOT_SELF, ct, (uint64_t)(uintptr_t)client_thread,
                        (uint64_t)(uintptr_t)(client_stack + sizeof client_stack - 8u)),
         "thread_spawn (client)");
    must(j_sc_create(csc, GT_PRIO, 10 * J_MS, 10 * J_MS), "sc_create (client)");
    must(j_sc_bind(csc, ct), "sc_bind (client)");
    uint64_t t0, now, ncpu;
    must(j_clock(&t0, &ncpu), "clock");
    while (phase != 1) {
        must(j_clock(&now, &ncpu), "clock");
        if (now - t0 >= GT_WAIT_NS) {
            hello_where();
            die("hello: the client's checks didn't end");
        }
        serve_one(J_MS);
    }
    /* hello's one serving thread stopped: the call waits in its queue. */
    must(j_thread_suspend(thread), "thread_suspend (hello)");
    phase = 2;
    sleep_ns(50u * J_MS);
    must(j_task_destroy(task), "task_destroy (hello)");
    must(j_clock(&t0, &ncpu), "clock");
    while (phase != 3) {
        must(j_clock(&now, &ncpu), "clock");
        if (now - t0 >= GT_WAIT_NS) {
            die("hello: the client's last call never ended");
        }
        sleep_ns(J_MS);
    }
    check(last_n == -1, "hello: a call to a killed translator didn't fail");
    j_puts("hello: killed mid-request ok\n");
    must(j_thread_kill(ct), "thread_kill (client)");
    must(j_cap_delete(ct), "cap_delete (client)");
    must(j_cap_delete(csc), "cap_delete (client's context)");
}
#endif

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
#ifdef GT_HELLO
    {
        node_cap = slot_new();
        must(j_cap_mint(root_ep_c, node_cap, J_R_SEND | J_R_GRANT, GT_BADGE_NODE), "cap_mint (node)");
        /* Its fsys_startup first, served as any other message. */
        uint64_t h0, hn, hc;
        must(j_clock(&h0, &hc), "clock");
        while (hello_control == 0) {
            must(j_clock(&hn, &hc), "clock");
            if (hn - h0 >= GT_WAIT_NS) {
                die("hello never sent fsys_startup");
            }
            serve_one(10u * J_MS);
        }
        j_puts("hello: fsys_startup ok\n");
        hello_thread = k.thread;
        hello_run(k.task, k.thread);
    }
#endif

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
