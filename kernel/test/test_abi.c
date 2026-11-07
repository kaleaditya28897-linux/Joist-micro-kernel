/* SPDX-License-Identifier: GPL-3.0-only */
/* test_abi.c - the system-call ABI (Phase 3 step 4; sys/abi.h): threads
 * driven through their saved registers only, as the kernel drives them. */
#include "abi.h"
#include "harness.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>

static struct joist_kernel k;
static struct abi_env env;
static joist_cspace root;
static struct vm_space nowhere; /* debug_write's space: nothing mapped */

static const struct vm_space *user = &nowhere; /* what space_of answers */

static const struct vm_space *space_of(void *ctx, uint32_t s)
{
    (void)ctx;
    (void)s;
    return user;
}

/* A little simulated memory, for the calls that read or write the caller's:
 * one writable user page at UPAGE, a read-only one after it. */
#define UPAGE 0x400000ull
#define UMEM_BASE 0x100000ull
static uint8_t umem[8 * 4096u] __attribute__((aligned(4096)));
static uint32_t unext;
static struct vm_space uspace;

static kerr uget(void *ctx, uint64_t *pa)
{
    (void)ctx;
    if (unext == sizeof umem / 4096u) {
        return KERR_NOMEM;
    }
    memset(&umem[unext * 4096u], 0, 4096u);
    *pa = UMEM_BASE + (uint64_t)unext++ * 4096u;
    return KERR_OK;
}

static void *uptr(void *ctx, uint64_t pa)
{
    (void)ctx;
    return &umem[pa - UMEM_BASE];
}

static uint8_t *with_user_memory(void)
{
    uint64_t pa;
    uint64_t ro;
    unext = 0;
    if (vm_create(&uspace, uget, uptr, NULL) != KERR_OK || uget(NULL, &pa) != KERR_OK ||
        uget(NULL, &ro) != KERR_OK ||
        vm_map(&uspace, UPAGE, pa, 4096u, VM_PAGE, VM_USER | VM_W) != KERR_OK ||
        vm_map(&uspace, UPAGE + 4096u, ro, 4096u, VM_PAGE, VM_USER) != KERR_OK) {
        abort();
    }
    user = &uspace;
    return uptr(NULL, pa);
}

static void out(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    (void)buf;
    (void)n;
}

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

static void setup(void)
{
    joist_init(&k);
    (void)joist_boot(&k, 64, &root);
    memset(&env, 0, sizeof env);
    user = &nowhere;
    env.k = &k;
    env.space_of = space_of;
    env.out = out;
}

/* Make a call from thread t: number and arguments into its registers. */
static int sys(uint32_t t, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    struct joist_regs *r = &k.th[t].regs;
    r->r[ABI_RAX] = nr;
    r->r[ABI_RDI] = a0;
    r->r[ABI_RSI] = a1;
    r->r[ABI_RDX] = a2;
    r->r[ABI_R10] = a3;
    return abi_syscall(&env, t);
}

JT_TEST(abi_unknown_call_changes_nothing_else)
{
    setup();
    joist_thread t;
    JT_REQUIRE(joist_thread_create(&k, root, &t) == JOIST_OK);
    for (unsigned i = 0; i < 16u; i++) {
        k.th[t].regs.r[i] = 0x1111u * (i + 1u);
    }
    struct joist_regs before = k.th[t].regs;
    before.r[ABI_RAX] = (uint64_t)0 - SYS_E_NOSYS;
    k.th[t].regs.r[ABI_RAX] = 7; /* unused */
    JT_CHECK(abi_syscall(&env, t) == ABI_DONE);
    JT_CHECK(memcmp(&k.th[t].regs, &before, sizeof before) == 0);
    k.th[t].regs.r[ABI_RAX] = 0x1234567890ull;
    JT_CHECK(abi_syscall(&env, t) == ABI_DONE &&
             k.th[t].regs.r[ABI_RAX] == (uint64_t)0 - SYS_E_NOSYS);
}

JT_TEST(abi_call_recv_reply_through_registers)
{
    setup();
    joist_thread c;
    joist_thread s;
    JT_REQUIRE(joist_thread_create(&k, root, &c) == JOIST_OK);
    JT_REQUIRE(joist_thread_create(&k, root, &s) == JOIST_OK);
    JT_REQUIRE(sys(s, ABI_OBJ_CREATE, JOIST_OBJ_ENDPOINT, 0, 0, 0) == ABI_DONE &&
               k.th[s].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(sys(s, ABI_CAP_MINT, 0, 1, JOIST_R_SEND, 42) == ABI_DONE &&
               k.th[s].regs.r[ABI_RAX] == JOIST_OK);
    for (unsigned i = 0; i < 4u; i++) {
        k.th[c].regs.r[ABI_R12 + i] = 100u + i;
    }
    k.th[c].regs.r[ABI_R9] = 0;
    JT_CHECK(sys(c, ABI_CALL, 1, JOIST_FOREVER, 0, 0) == ABI_WAITING);
    JT_CHECK(sys(s, ABI_RECV, 0, JOIST_FOREVER, 0, 0) == ABI_DONE);
    JT_CHECK(k.th[s].regs.r[ABI_RAX] == JOIST_OK && k.th[s].regs.r[ABI_RDX] == 42);
    JT_CHECK(k.th[s].regs.r[ABI_R12] == 100 && k.th[s].regs.r[ABI_R15] == 103);
    k.th[s].regs.r[ABI_R12] = 7;
    k.th[s].regs.r[ABI_R9] = 0;
    JT_CHECK(sys(s, ABI_REPLY, 0, 0, 0, 0) == ABI_DONE && k.th[s].regs.r[ABI_RAX] == JOIST_OK);
    /* The caller's outputs are written when it is about to run again. */
    JT_CHECK(env.pending[c]);
    abi_complete(&env, c);
    JT_CHECK(!env.pending[c] && k.th[c].regs.r[ABI_RAX] == JOIST_OK &&
             k.th[c].regs.r[ABI_R12] == 7);
    JT_CHECK(clean());
}

JT_TEST(abi_capabilities_in_r9)
{
    setup();
    struct joist_msg m = {0};
    m.ncaps = 2;
    m.cap[0] = 5;
    m.mask[0] = JOIST_R_SEND;
    m.cap[1] = JOIST_SLOT_NONE;
    struct joist_msg back;
    abi_caps_unpack(abi_caps_pack(&m), &back);
    JT_CHECK(back.ncaps == 2 && back.cap[0] == 5 && back.mask[0] == JOIST_R_SEND);
    JT_CHECK(back.cap[1] == JOIST_CSPACE_SLOTS); /* "none" sent is out of range */
    /* Through a call: the server names a receive slot, the client sends a
     * notification capability, and the server's R9 says where it landed. */
    joist_thread c;
    joist_thread s;
    JT_REQUIRE(joist_thread_create(&k, root, &c) == JOIST_OK);
    JT_REQUIRE(joist_thread_create(&k, root, &s) == JOIST_OK);
    JT_REQUIRE(sys(s, ABI_OBJ_CREATE, JOIST_OBJ_ENDPOINT, 0, 0, 0) == ABI_DONE);
    JT_REQUIRE(sys(c, ABI_OBJ_CREATE, JOIST_OBJ_NOTIFICATION, 3, 0, 0) == ABI_DONE);
    JT_REQUIRE(sys(s, ABI_RECV_SLOTS, 9, 10, 1, 0) == ABI_DONE && k.th[s].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(sys(s, ABI_RECV, 0, JOIST_FOREVER, 0, 0) == ABI_WAITING);
    struct joist_msg send = {0};
    send.ncaps = 1;
    send.cap[0] = 3;
    send.mask[0] = JOIST_R_SEND;
    k.th[c].regs.r[ABI_R9] = abi_caps_pack(&send);
    JT_REQUIRE(sys(c, ABI_CALL, 0, JOIST_FOREVER, 0, 0) == ABI_WAITING);
    abi_complete(&env, s);
    struct joist_msg got;
    abi_caps_unpack(k.th[s].regs.r[ABI_R9], &got);
    JT_CHECK(got.ncaps == 1 && got.cap[0] == 9);
    struct joist_cap cap;
    JT_CHECK(joist_cap_read(&k, root, 9, &cap) == JOIST_OK && cap.rights == JOIST_R_SEND);
    JT_CHECK(clean());
}

JT_TEST(abi_wide_slot_arguments_are_out_of_range)
{
    setup();
    joist_thread t;
    JT_REQUIRE(joist_thread_create(&k, root, &t) == JOIST_OK);
    JT_REQUIRE(sys(t, ABI_OBJ_CREATE, JOIST_OBJ_ENDPOINT, 0, 0, 0) == ABI_DONE);
    /* 2^32 + 0 must not become slot 0. */
    JT_CHECK(sys(t, ABI_CAP_DELETE, (1ull << 32), 0, 0, 0) == ABI_DONE &&
             k.th[t].regs.r[ABI_RAX] == JOIST_E_RANGE);
    struct joist_cap c;
    JT_CHECK(joist_cap_read(&k, root, 0, &c) == JOIST_OK);
    JT_CHECK(sys(t, ABI_SC_CREATE, 5, 256, 1, 1) == ABI_DONE &&
             k.th[t].regs.r[ABI_RAX] == JOIST_E_RIGHTS); /* a priority past 255 */
    JT_CHECK(clean());
}

JT_TEST(abi_spawn_starts_at_pc_and_sp)
{
    setup();
    joist_thread t;
    JT_REQUIRE(joist_thread_create(&k, root, &t) == JOIST_OK);
    JT_REQUIRE(sys(t, ABI_TASK_CREATE, 10, 0, 0, 0) == ABI_DONE && k.th[t].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(sys(t, ABI_THREAD_SPAWN, 0, 1, 0x401000, 0x7ff000) == ABI_DONE &&
               k.th[t].regs.r[ABI_RAX] == JOIST_OK);
    uint32_t nt = k.obj[k.cs[root].slot[1].obj - 1u].target - 1u;
    JT_CHECK(k.th[nt].state == JOIST_T_RUNNING && k.th[nt].regs.pc == 0x401000 &&
             k.th[nt].regs.sp == 0x7ff000 && k.th[nt].regs.r[ABI_RAX] == 0);
    JT_CHECK(clean());
}

JT_TEST(abi_exit)
{
    setup();
    joist_thread t;
    JT_REQUIRE(joist_thread_create(&k, root, &t) == JOIST_OK);
    /* From another task: only that thread ends. */
    JT_REQUIRE(sys(t, ABI_TASK_CREATE, 10, 0, 0, 0) == ABI_DONE);
    JT_REQUIRE(sys(t, ABI_THREAD_SPAWN, 0, 1, 0x401000, 0x7ff000) == ABI_DONE);
    uint32_t nt = k.obj[k.cs[root].slot[1].obj - 1u].target - 1u;
    JT_CHECK(sys(nt, ABI_EXIT, 3, 0, 0, 0) == ABI_GONE);
    JT_CHECK(k.th[nt].state == JOIST_T_FREE && !env.finished);
    /* From the root task: the run is over. */
    JT_CHECK(sys(t, ABI_EXIT, 5, 0, 0, 0) == ABI_GONE);
    JT_CHECK(env.finished && env.exit_code == 5);
    JT_CHECK(clean());
}

JT_TEST(abi_cap_read_reports_frames)
{
    setup();
    joist_thread t;
    JT_REQUIRE(joist_thread_create(&k, root, &t) == JOIST_OK);
    JT_REQUIRE(joist_boot_frame(&k, root, 4, 0x100, 37) == JOIST_OK);
    JT_CHECK(sys(t, ABI_CAP_READ, 4, 0, 0, 0) == ABI_DONE);
    JT_CHECK(k.th[t].regs.r[ABI_RAX] == JOIST_OK && (k.th[t].regs.r[ABI_RDX] & 0xffu) == JOIST_OBJ_FRAME);
    JT_CHECK(k.th[t].regs.r[ABI_R10] == 37);
    JT_CHECK(clean());
}

/* ---- decision 0022: the batched copy and revoke by kill ---- */

JT_TEST(abi_cap_copy_batch_and_obj_kill)
{
    setup();
    joist_thread t;
    JT_REQUIRE(joist_thread_create(&k, root, &t) == JOIST_OK);
    JT_REQUIRE(sys(t, ABI_TASK_CREATE, 4, 0, 0, 0) == ABI_DONE && k.th[t].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 2) == JOIST_OK);
    JT_REQUIRE(joist_cap_derive(&k, root, 2, 3, JOIST_R_SEND) == JOIST_OK);              /* no GRANT */
    JT_REQUIRE(joist_cap_derive(&k, root, 2, 4, JOIST_R_SEND | JOIST_R_GRANT) == JOIST_OK);
    JT_REQUIRE(joist_boot_frame(&k, root, 5, 0x100, 4) == JOIST_OK);
    /* Slots 2-5 into the task: RECV skipped (no flag), 3 skipped (no
     * GRANT), 4 and 5 copied with the mask, in R10; flags in R8. */
    k.th[t].regs.r[ABI_R8] = 0;
    JT_CHECK(sys(t, ABI_CAP_COPY_BATCH, 0, 2, 4, JOIST_R_ALL) == ABI_DONE);
    JT_CHECK(k.th[t].regs.r[ABI_RAX] == JOIST_OK && k.th[t].regs.r[ABI_RDX] == 4 &&
             k.th[t].regs.r[ABI_R8] == 0x3);
    uint32_t child = k.obj[k.cs[root].slot[0].obj - 1u].target - 1u;
    JT_CHECK(k.cs[child].slot[2].obj == 0 && k.cs[child].slot[3].obj == 0);
    JT_CHECK(k.cs[child].slot[4].obj == k.cs[root].slot[4].obj && k.cs[child].slot[5].rights ==
                                                                      (JOIST_R_SEND | JOIST_R_RECV |
                                                                       JOIST_R_KILL | JOIST_R_GRANT));
    /* Again: stops at once on slot 4, now occupied; an unknown flag, or 33. */
    k.th[t].regs.r[ABI_R8] = JOIST_COPY_RECV;
    JT_CHECK(sys(t, ABI_CAP_COPY_BATCH, 0, 2, 4, JOIST_R_ALL) == ABI_DONE);
    JT_CHECK(k.th[t].regs.r[ABI_RAX] == JOIST_E_OCCUPIED && k.th[t].regs.r[ABI_RDX] == 2 &&
             k.th[t].regs.r[ABI_R8] == 0x2 && k.cs[child].slot[2].obj != 0);
    k.th[t].regs.r[ABI_R8] = 1ull << 40;
    JT_CHECK(sys(t, ABI_CAP_COPY_BATCH, 0, 6, 1, JOIST_R_ALL) == ABI_DONE &&
             k.th[t].regs.r[ABI_RAX] == JOIST_E_RANGE);
    k.th[t].regs.r[ABI_R8] = 0;
    JT_CHECK(sys(t, ABI_CAP_COPY_BATCH, 0, 6, JOIST_COPY_BATCH + 1u, JOIST_R_ALL) == ABI_DONE &&
             k.th[t].regs.r[ABI_RAX] == JOIST_E_RANGE && k.th[t].regs.r[ABI_RDX] == 0);
    /* Kill the frame from the root: the child's copy is dead too. */
    JT_CHECK(sys(t, ABI_OBJ_KILL, 2, 0, 0, 0) == ABI_DONE && k.th[t].regs.r[ABI_RAX] == JOIST_E_TYPE);
    JT_CHECK(sys(t, ABI_OBJ_KILL, 5, 0, 0, 0) == ABI_DONE && k.th[t].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(sys(t, ABI_OBJ_KILL, 5, 0, 0, 0) == ABI_DONE && k.th[t].regs.r[ABI_RAX] == JOIST_E_GONE);
    JT_CHECK(joist_map(&k, child, 5, 0, 1, JOIST_SLOT_SELF, 0x400, 0) == JOIST_E_GONE);
    JT_CHECK(clean());
}

JT_TEST(abi_wait_completes_on_signal)
{
    setup();
    joist_thread w;
    joist_thread s;
    JT_REQUIRE(joist_thread_create(&k, root, &w) == JOIST_OK);
    JT_REQUIRE(joist_thread_create(&k, root, &s) == JOIST_OK);
    JT_REQUIRE(sys(w, ABI_OBJ_CREATE, JOIST_OBJ_NOTIFICATION, 0, 0, 0) == ABI_DONE);
    JT_REQUIRE(sys(w, ABI_CAP_MINT, 0, 1, JOIST_R_SEND, 0x8) == ABI_DONE);
    JT_CHECK(sys(w, ABI_WAIT, 0, JOIST_FOREVER, 0, 0) == ABI_WAITING);
    JT_CHECK(sys(s, ABI_SIGNAL, 1, 0, 0, 0) == ABI_DONE && k.th[s].regs.r[ABI_RAX] == JOIST_OK);
    abi_complete(&env, w);
    JT_CHECK(k.th[w].regs.r[ABI_RAX] == JOIST_OK && k.th[w].regs.r[ABI_RDX] == 0x8);
    JT_CHECK(clean());
}

/* ---- Phase 3 step 6: a thread's registers through the caller's memory ---- */

JT_TEST(abi_thread_regs_read_and_write)
{
    setup();
    uint8_t *mem = with_user_memory();
    joist_thread a;
    joist_thread b;
    JT_REQUIRE(joist_thread_create(&k, root, &a) == JOIST_OK);
    JT_REQUIRE(sys(a, ABI_TASK_CREATE, 10, 2, 0, 0) == ABI_DONE &&
               k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(sys(a, ABI_THREAD_SPAWN, 2, 3, 0x401000, 0x7000) == ABI_DONE &&
               k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    b = k.obj[k.cs[root].slot[3].obj - 1u].target - 1u;
    for (unsigned i = 0; i < JOIST_REGS; i++) {
        k.th[b].regs.r[i] = 0x100u + i;
    }
    k.th[b].regs.pc = 0x401000;
    k.th[b].regs.sp = 0x7000;
    k.th[b].regs.tls = 0x5000;
    struct joist_regs want = k.th[b].regs;
    JT_CHECK(sys(a, ABI_THREAD_READ_REGS, 3, UPAGE + 8, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(memcmp(mem + 8, &want, sizeof want) == 0);
    /* Write: what the caller's memory holds becomes the record, whole. */
    struct joist_regs in = want;
    in.pc = 0x402000;
    in.tls = 0xffff800000000000ull; /* anything is accepted here; the exit checks */
    in.r[ABI_R15] = 77;
    memcpy(mem + 100, &in, sizeof in);
    JT_CHECK(sys(a, ABI_THREAD_WRITE_REGS, 3, UPAGE + 100, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(memcmp(&k.th[b].regs, &in, sizeof in) == 0);
    JT_CHECK(clean());
}

/* A thread writing its own record (how glibc sets its TLS base, 0010):
 * read it, change the TLS base, write it back. It resumes at the record's
 * pc (the read's return, on a real CPU: user/schedroot's "own tls" test),
 * every register as written but the call's outputs, which come after: RAX
 * the result, RDX unchanged (no value returned). */
JT_TEST(abi_thread_regs_own)
{
    setup();
    uint8_t *mem = with_user_memory();
    joist_thread a;
    joist_thread b;
    JT_REQUIRE(joist_thread_create(&k, root, &a) == JOIST_OK);
    JT_REQUIRE(sys(a, ABI_TASK_CREATE, 10, 2, 0, 0) == ABI_DONE &&
               k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(sys(a, ABI_THREAD_SPAWN, 2, 3, 0x401000, 0x7000) == ABI_DONE &&
               k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    b = k.obj[k.cs[root].slot[3].obj - 1u].target - 1u;
    /* b's own capability, in its task's slot 0. */
    JT_REQUIRE(sys(a, ABI_CAP_GIVE, 2, 3, 0, JOIST_R_SEND | JOIST_R_GRANT) == ABI_DONE &&
               k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    k.th[b].regs.r[ABI_R15] = 1234;
    JT_REQUIRE(sys(b, ABI_THREAD_READ_REGS, 0, UPAGE + 8, 0, 0) == ABI_DONE &&
               k.th[b].regs.r[ABI_RAX] == JOIST_OK);
    struct joist_regs rec;
    memcpy(&rec, mem + 8, sizeof rec);
    JT_CHECK(rec.pc == 0x401000 && rec.r[ABI_R15] == 1234);
    rec.tls = 0x6000;
    memcpy(mem + 8, &rec, sizeof rec);
    JT_CHECK(sys(b, ABI_THREAD_WRITE_REGS, 0, UPAGE + 8, 0, 0) == ABI_DONE &&
             k.th[b].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(k.th[b].regs.tls == 0x6000 && k.th[b].regs.pc == 0x401000 && k.th[b].regs.sp == 0x7000);
    JT_CHECK(k.th[b].regs.r[ABI_R15] == 1234 && k.th[b].regs.r[ABI_RDI] == 0);
    JT_CHECK(clean());
}

JT_TEST(abi_thread_regs_refusals)
{
    setup();
    uint8_t *mem = with_user_memory();
    joist_thread a;
    joist_thread b;
    JT_REQUIRE(joist_thread_create(&k, root, &a) == JOIST_OK);
    JT_REQUIRE(sys(a, ABI_TASK_CREATE, 10, 2, 0, 0) == ABI_DONE &&
               k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(sys(a, ABI_THREAD_SPAWN, 2, 3, 0x401000, 0x7000) == ABI_DONE &&
               k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    b = k.obj[k.cs[root].slot[3].obj - 1u].target - 1u;
    k.th[b].regs.pc = 0x401000;
    struct joist_regs before = k.th[b].regs;
    memset(mem, 0x5a, 4096u);
    /* The capability's errors come first, whatever the pointer. */
    JT_CHECK(sys(a, ABI_THREAD_READ_REGS, 4, 0, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_EMPTY);
    JT_CHECK(sys(a, ABI_THREAD_WRITE_REGS, 4, 0, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_EMPTY);
    JT_CHECK(sys(a, ABI_THREAD_WRITE_REGS, 1ull << 32 | 3u, UPAGE, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE);
    /* A record that doesn't fit the caller's memory: E_RANGE, nothing done.
     * Unmapped; the kernel half; running off the writable page into the
     * read-only one (for a read: nothing written, not even the first part). */
    uint64_t bad[] = {0x1000, 0xffff800000000000ull, UPAGE + 4096u - 8u, UINT64_MAX};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        JT_CHECK(sys(a, ABI_THREAD_READ_REGS, 3, bad[i], 0, 0) == ABI_DONE &&
                 k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE);
    }
    for (unsigned i = 0; i < 4096u; i++) {
        JT_CHECK(mem[i] == 0x5a);
    }
    /* A write whose record lies partly on an unmapped page. */
    JT_CHECK(sys(a, ABI_THREAD_WRITE_REGS, 3, UPAGE + 2 * 4096u - 8u, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE);
    JT_CHECK(sys(a, ABI_THREAD_WRITE_REGS, 3, 0x1000, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE);
    JT_CHECK(memcmp(&k.th[b].regs, &before, sizeof before) == 0);
    /* A record read from a read-only page is fine. */
    JT_CHECK(sys(a, ABI_THREAD_WRITE_REGS, 3, UPAGE + 4096u, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(k.th[b].regs.pc == 0);
    /* Without CONTROL on the thread: refused, both ways. */
    JT_REQUIRE(joist_cap_derive(&k, root, 3, 5, JOIST_R_GRANT) == JOIST_OK);
    JT_CHECK(sys(a, ABI_THREAD_READ_REGS, 5, UPAGE, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RIGHTS);
    JT_CHECK(sys(a, ABI_THREAD_WRITE_REGS, 5, UPAGE, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RIGHTS);
    JT_CHECK(clean());
}

/* ---- Phase 4 step 2: map_query through the caller's memory (0020) ---- */

JT_TEST(abi_map_query_into_the_callers_memory)
{
    setup();
    uint8_t *mem = with_user_memory();
    joist_thread a;
    JT_REQUIRE(joist_thread_create(&k, root, &a) == JOIST_OK);
    JT_REQUIRE(sys(a, ABI_TASK_CREATE, 10, 2, 0, 0) == ABI_DONE &&
               k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    memset(mem, 0x5a, 4096u);
    /* Nothing mapped: four zeros, written at the caller's pointer. */
    k.th[a].regs.r[ABI_R8] = UPAGE + 16u;
    JT_CHECK(sys(a, ABI_MAP_QUERY, 2, 0x400, 4, JOIST_SLOT_NONE) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    for (unsigned i = 0; i < 4096u; i++) {
        JT_CHECK(mem[i] == (i >= 16u && i < 16u + 4u * 8u ? 0 : 0x5a));
    }
    /* Refusals write nothing: too many pages, memory that can't take it
     * whole (unmapped; running into the read-only page), an empty slot. */
    memset(mem, 0x5a, 4096u);
    JT_CHECK(sys(a, ABI_MAP_QUERY, 2, 0x400, JOIST_QUERY_MAX + 1u, JOIST_SLOT_NONE) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE);
    uint64_t bad[] = {0x1000, UPAGE + 4096u - 8u};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        k.th[a].regs.r[ABI_R8] = bad[i];
        JT_CHECK(sys(a, ABI_MAP_QUERY, 2, 0x400, 4, JOIST_SLOT_NONE) == ABI_DONE &&
                 k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE);
    }
    k.th[a].regs.r[ABI_R8] = UPAGE;
    JT_CHECK(sys(a, ABI_MAP_QUERY, 7, 0x400, 4, JOIST_SLOT_NONE) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_EMPTY);
    for (unsigned i = 0; i < 4096u; i++) {
        JT_CHECK(mem[i] == 0x5a);
    }
    JT_CHECK(clean());
}

/* ---- Phase 3 step 7: continue never sweeps more than the kernel's limit ---- */

/* ---- Phase 5 step 3: the region calls (0030) ---- */

/* Each argument reaches its parameter: the model's tests cover what the
 * calls do; these, that a register mix-up would show. */
JT_TEST(abi_region_calls_take_their_registers)
{
    setup();
    joist_thread a;
    JT_REQUIRE(joist_thread_create(&k, root, &a) == JOIST_OK);
    JT_REQUIRE(sys(a, ABI_TASK_CREATE, 10, 2, 0, 0) == ABI_DONE &&
               k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(joist_boot_frame(&k, root, 4, 0x100, 8) == JOIST_OK);
    const uint64_t lo = 2u * JOIST_PT_ENTRIES;
    /* create: the task, lo, tables, the ceiling; the slot in R8. */
    k.th[a].regs.r[ABI_R8] = 5;
    JT_CHECK(sys(a, ABI_REGION_CREATE, 2, lo + 1u, 1, JOIST_MAP_W) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE); /* lo not on a table */
    JT_CHECK(sys(a, ABI_REGION_CREATE, 2, lo, 0, JOIST_MAP_W) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE); /* no tables */
    JT_CHECK(sys(a, ABI_REGION_CREATE, 2, lo, 1, 0x80) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE); /* an unknown ceiling bit */
    k.th[a].regs.r[ABI_R8] = 1ull << 40;
    JT_CHECK(sys(a, ABI_REGION_CREATE, 2, lo, 1, JOIST_MAP_W) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE); /* a wide slot */
    k.th[a].regs.r[ABI_R8] = 5;
    JT_CHECK(sys(a, ABI_REGION_CREATE, 2, lo, 1, JOIST_MAP_W) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_OK && k.cs[root].slot[5].type == JOIST_OBJ_REGION);
    /* add: the region, the frame, an offset, pages; the extent in RDX. */
    JT_CHECK(sys(a, ABI_REGION_ADD, 5, 4, 6, 3) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE); /* past the frame */
    JT_CHECK(sys(a, ABI_REGION_ADD, 5, 4, 2, 3) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_OK && k.th[a].regs.r[ABI_RDX] == 0);
    JT_CHECK(sys(a, ABI_REGION_ADD, 5, 4, 0, 1) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_OK && k.th[a].regs.r[ABI_RDX] == 1);
    /* map: the region, the extent, a page in it, pages; va in R8, perms in
     * R9. No table pages are lent to the task, so a call that passes every
     * check is E_NOMEM. */
    k.th[a].regs.r[ABI_R8] = lo;
    k.th[a].regs.r[ABI_R9] = JOIST_MAP_X;
    JT_CHECK(sys(a, ABI_REGION_MAP, 5, 0, 0, 1) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RIGHTS); /* X beyond the ceiling */
    k.th[a].regs.r[ABI_R9] = JOIST_MAP_W;
    JT_CHECK(sys(a, ABI_REGION_MAP, 5, 2, 0, 1) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE); /* no extent 2 */
    JT_CHECK(sys(a, ABI_REGION_MAP, 5, 0, 1, 3) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE); /* past the extent */
    k.th[a].regs.r[ABI_R8] = lo + JOIST_PT_ENTRIES;
    JT_CHECK(sys(a, ABI_REGION_MAP, 5, 0, 0, 1) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE); /* outside the region */
    k.th[a].regs.r[ABI_R8] = lo + 7u;
    JT_CHECK(sys(a, ABI_REGION_MAP, 5, 0, 0, 3) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_NOMEM);
    /* unmap and protect: the region, va, pages (and perms). */
    JT_CHECK(sys(a, ABI_REGION_UNMAP, 5, lo - 1u, 2, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE);
    JT_CHECK(sys(a, ABI_REGION_UNMAP, 5, lo, 2, 0) == ABI_DONE && k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(sys(a, ABI_REGION_PROTECT, 5, lo, 2, JOIST_MAP_X) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RIGHTS);
    JT_CHECK(sys(a, ABI_REGION_PROTECT, 5, lo, 2, JOIST_MAP_W) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    /* Through a capability with MAP alone, as a task holds one. */
    JT_REQUIRE(joist_cap_derive(&k, root, 5, 6, JOIST_R_MAP) == JOIST_OK);
    JT_CHECK(sys(a, ABI_REGION_ADD, 6, 4, 0, 1) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RIGHTS);
    JT_CHECK(sys(a, ABI_REGION_UNMAP, 6, lo, 1, 0) == ABI_DONE && k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    /* Killed: every capability to it is dead. */
    JT_CHECK(sys(a, ABI_OBJ_KILL, 5, 0, 0, 0) == ABI_DONE && k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(sys(a, ABI_REGION_UNMAP, 6, lo, 1, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_GONE);
    JT_CHECK(clean());
}

JT_TEST(abi_continue_is_bounded_by_the_preemption_limit)
{
    setup();
    joist_thread t;
    JT_REQUIRE(joist_thread_create(&k, root, &t) == JOIST_OK);
    JT_REQUIRE(sys(t, ABI_TASK_CREATE, 10, 0, 0, 0) == ABI_DONE && k.th[t].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(sys(t, ABI_OBJ_CREATE, JOIST_OBJ_ENDPOINT, 1, 0, 0) == ABI_DONE &&
               k.th[t].regs.r[ABI_RAX] == JOIST_OK);
    for (uint64_t i = 0; i < 20u; i++) {
        JT_REQUIRE(sys(t, ABI_CAP_GIVE, 0, 1, i, JOIST_R_SEND) == ABI_DONE &&
                   k.th[t].regs.r[ABI_RAX] == JOIST_OK);
    }
    uint32_t victim = k.obj[k.cs[root].slot[0].obj - 1u].target - 1u;
    k.preempt = 2;
    JT_CHECK(sys(t, ABI_TASK_DESTROY, 0, 0, 0, 0) == ABI_DONE && k.th[t].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(k.npending == 1 && k.cs[victim].dying);
    /* Whatever the caller asks for (0 would mean "all of it", and so would
     * a large number), a call sweeps at most the limit; smaller is honoured. */
    uint64_t asks[] = {0, 1000, UINT64_MAX, 1, 2, 3};
    unsigned calls = 0;
    while (k.npending != 0 && calls < 1000u) {
        uint64_t ask = asks[calls % 6u];
        uint32_t before = k.cs[victim].sweep;
        JT_CHECK(sys(t, ABI_CONTINUE, ask, 0, 0, 0) == ABI_DONE && k.th[t].regs.r[ABI_RAX] == JOIST_OK);
        /* (Within the first leaf the cursor counts slots; past it, an absent
         * leaf is one unit of work and moves it by a whole leaf.) */
        uint32_t swept = k.npending != 0 && k.cs[victim].sweep <= JOIST_CNODE_LEAF
                             ? k.cs[victim].sweep - before : 0;
        JT_CHECK(swept <= (ask == 1 ? 1u : 2u));
        JT_CHECK(k.th[t].regs.r[ABI_RDX] == (k.npending != 0 ? 1u : 0u));
        JT_CHECK(clean());
        calls++;
    }
    JT_CHECK(k.npending == 0 && calls >= JOIST_CNODE_LEAF / 2u);
    JT_CHECK(k.preempt == 2);
}

/* ---- Phase 3 step 8: the direct switch is reported, and only when sure ---- */

JT_TEST(abi_reports_the_direct_switch)
{
    setup();
    joist_thread c;
    joist_thread s;
    JT_REQUIRE(joist_thread_create(&k, root, &c) == JOIST_OK);
    JT_REQUIRE(joist_thread_create(&k, root, &s) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_REQUIRE(joist_sc_create(&k, root, 1, 5, 100, 100) == JOIST_OK &&
               joist__sc_bind_thread(&k, k.cs[root].slot[1].obj - 1u, c) == JOIST_OK);
    /* A call that finds nobody waiting: nobody to switch to. */
    k.th[c].regs.r[ABI_R9] = 0;
    env.handover = 7;
    JT_CHECK(sys(c, ABI_CALL, 0, JOIST_FOREVER, 0, 0) == ABI_WAITING);
    JT_CHECK(env.handover == JOIST_MAX_THREADS);
    /* The passive server takes it: it goes on (no switch), on c's context. */
    JT_CHECK(sys(s, ABI_RECV, 0, JOIST_FOREVER, 0, 0) == ABI_DONE);
    JT_CHECK(env.handover == JOIST_MAX_THREADS);
    /* Its ReplyRecv hands the context back: straight to c. */
    k.th[s].regs.r[ABI_R9] = 0;
    JT_CHECK(sys(s, ABI_REPLY_RECV, 0, JOIST_FOREVER, 0, 0) == ABI_WAITING);
    JT_CHECK(env.handover == c);
    abi_complete(&env, c);
    /* And c's next call, with s waiting: straight to s. */
    JT_CHECK(sys(c, ABI_CALL, 0, JOIST_FOREVER, 0, 0) == ABI_WAITING);
    JT_CHECK(env.handover == s);
    abi_complete(&env, s);
    /* With a timeout on the wait, the scheduler is asked. */
    JT_CHECK(sys(s, ABI_REPLY_RECV, 0, 1000, 0, 0) == ABI_WAITING);
    JT_CHECK(env.handover == JOIST_MAX_THREADS);
    /* Any other call clears it. */
    env.handover = 7;
    JT_CHECK(sys(c, ABI_CAP_DELETE, 9, 0, 0, 0) == ABI_DONE);
    JT_CHECK(env.handover == JOIST_MAX_THREADS);
    JT_CHECK(clean());
}

JT_TEST(abi_marks_threads_whose_registers_all_count)
{
    setup();
    uint8_t *mem = with_user_memory();
    joist_thread a;
    JT_REQUIRE(joist_thread_create(&k, root, &a) == JOIST_OK);
    JT_REQUIRE(sys(a, ABI_TASK_CREATE, 10, 2, 0, 0) == ABI_DONE);
    memset(env.exact, 0, sizeof env.exact);
    memset(env.fresh, 0, sizeof env.fresh);
    JT_REQUIRE(sys(a, ABI_THREAD_SPAWN, 2, 3, 0x401000, 0x7000) == ABI_DONE &&
               k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    uint32_t b = k.obj[k.cs[root].slot[3].obj - 1u].target - 1u;
    JT_CHECK(env.exact[b] == 1 && env.exact[a] == 0); /* a new thread starts exactly as set */
    /* and from the machine's initial state of what its record doesn't hold */
    JT_CHECK(env.fresh[b] == 1 && env.fresh[a] == 0);
    env.fresh[b] = 0; /* as the kernel clears it, on its first way out */
    env.exact[b] = 0; /* as if it had made a system call since */
    memcpy(mem, &k.th[b].regs, sizeof k.th[b].regs);
    /* A refused write changes nothing; a write that happens marks it. */
    JT_CHECK(sys(a, ABI_THREAD_WRITE_REGS, 3, 0x1000, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_E_RANGE && env.exact[b] == 0);
    JT_CHECK(sys(a, ABI_THREAD_READ_REGS, 3, UPAGE + 512, 0, 0) == ABI_DONE && env.exact[b] == 0);
    JT_CHECK(sys(a, ABI_THREAD_WRITE_REGS, 3, UPAGE, 0, 0) == ABI_DONE &&
             k.th[a].regs.r[ABI_RAX] == JOIST_OK && env.exact[b] == 1 && env.exact[a] == 0);
    JT_CHECK(env.fresh[b] == 0); /* written registers aren't a new thread: its FPU state stays */
    JT_CHECK(clean());
}

/* ---- Phase 3's exit: the counters, for the memory-growth test ---- */

JT_TEST(abi_debug_counters)
{
    setup();
    uint8_t *mem = with_user_memory();
    joist_thread r;
    joist_thread x;
    JT_REQUIRE(joist_thread_create(&k, root, &r) == JOIST_OK);
    JT_REQUIRE(sys(r, ABI_TASK_CREATE, 10, 2, 0, 0) == ABI_DONE && k.th[r].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(sys(r, ABI_THREAD_SPAWN, 2, 3, 0x401000, 0x7000) == ABI_DONE);
    JT_REQUIRE(sys(r, ABI_OBJ_CREATE, JOIST_OBJ_ENDPOINT, 4, 0, 0) == ABI_DONE);
    joist_thread y; /* one waiting, so that a thread in use isn't only a running one */
    JT_REQUIRE(joist_thread_create(&k, root, &y) == JOIST_OK);
    JT_REQUIRE(joist_recv(&k, y, 4, JOIST_FOREVER) == JOIST_BLOCKED);
    struct abi_counters c;
    /* Only the root task may ask (joist_boot made `root` the root). */
    JT_CHECK(sys(r, ABI_DEBUG_COUNTERS, UPAGE, 0, 0, 0) == ABI_DONE && k.th[r].regs.r[ABI_RAX] == JOIST_OK);
    memcpy(&c, mem, sizeof c);
    JT_CHECK(c.objects == k.objects_live && c.threads == 3 && c.spaces == 2 && c.pending == 0);
    JT_CHECK(c.table_pool == 0 && c.frames_free == 0); /* no machine here */
    x = k.obj[k.cs[root].slot[3].obj - 1u].target - 1u;
    memset(mem, 0x5a, 64);
    JT_CHECK(sys(x, ABI_DEBUG_COUNTERS, UPAGE, 0, 0, 0) == ABI_DONE &&
             k.th[x].regs.r[ABI_RAX] == JOIST_E_RIGHTS && mem[0] == 0x5a);
    /* Where the record can't be written whole: E_RANGE, nothing written. */
    JT_CHECK(sys(r, ABI_DEBUG_COUNTERS, UPAGE + 4096u - 8u, 0, 0, 0) == ABI_DONE &&
             k.th[r].regs.r[ABI_RAX] == JOIST_E_RANGE);
    /* The counts follow: the task destroyed, everything it held goes. */
    JT_REQUIRE(sys(r, ABI_TASK_DESTROY, 2, 0, 0, 0) == ABI_DONE);
    JT_CHECK(sys(r, ABI_DEBUG_COUNTERS, UPAGE, 0, 0, 0) == ABI_DONE);
    memcpy(&c, mem, sizeof c);
    JT_CHECK(c.threads == 2 && c.spaces == 1 && c.objects == k.objects_live);
    JT_CHECK(clean());
}

/* ---- Phase 3's exit: the IPC buffer's words, through the registers ---- */

static int copy_calls;
static int copy_ok(void *ctx, uint32_t fs, uint64_t fva, uint32_t ts, uint64_t tva, uint32_t words)
{
    (void)ctx;
    (void)fs;
    (void)fva;
    (void)ts;
    (void)tva;
    (void)words;
    copy_calls++;
    return 1;
}

JT_TEST(abi_ipc_buffer_words_in_r9)
{
    setup();
    k.hooks.copy_msg = copy_ok;
    copy_calls = 0;
    joist_thread c;
    joist_thread s;
    JT_REQUIRE(joist_thread_create(&k, root, &c) == JOIST_OK);
    JT_REQUIRE(joist_thread_create(&k, root, &s) == JOIST_OK);
    JT_REQUIRE(joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, 0) == JOIST_OK);
    JT_CHECK(sys(c, ABI_IPC_BUFFER, 0x20001, 0, 0, 0) == ABI_DONE && k.th[c].regs.r[ABI_RAX] == JOIST_E_RANGE);
    JT_CHECK(sys(c, ABI_IPC_BUFFER, 0x20000, 0, 0, 0) == ABI_DONE && k.th[c].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(sys(s, ABI_IPC_BUFFER, 0x30000, 0, 0, 0) == ABI_DONE && k.th[s].regs.r[ABI_RAX] == JOIST_OK);
    JT_REQUIRE(sys(s, ABI_RECV, 0, JOIST_FOREVER, 0, 0) == ABI_WAITING);
    /* 513 words: more than a buffer holds; refused at the send. */
    k.th[c].regs.r[ABI_R9] = 513ull << ABI_LEN_SHIFT;
    JT_CHECK(sys(c, ABI_CALL, 0, JOIST_FOREVER, 0, 0) == ABI_DONE && k.th[c].regs.r[ABI_RAX] == JOIST_E_RANGE);
    /* 300 words: they go, and the receiver's R9 says how many. */
    k.th[c].regs.r[ABI_R9] = 300ull << ABI_LEN_SHIFT;
    JT_CHECK(sys(c, ABI_CALL, 0, JOIST_FOREVER, 0, 0) == ABI_WAITING && copy_calls == 1);
    abi_complete(&env, s);
    JT_CHECK(((k.th[s].regs.r[ABI_R9] >> ABI_LEN_SHIFT) & ABI_LEN_MASK) == 300 &&
             (k.th[s].regs.r[ABI_R9] & ABI_LOST_BIT) == 0);
    /* The reply's words lost (the caller dropped its buffer): bit 63. */
    k.th[c].ipc_buf = 0;
    k.th[s].regs.r[ABI_R9] = 5ull << ABI_LEN_SHIFT;
    JT_CHECK(sys(s, ABI_REPLY, 0, 0, 0, 0) == ABI_DONE && k.th[s].regs.r[ABI_RAX] == JOIST_OK);
    abi_complete(&env, c);
    JT_CHECK((k.th[c].regs.r[ABI_R9] & ABI_LOST_BIT) != 0 &&
             ((k.th[c].regs.r[ABI_R9] >> ABI_LEN_SHIFT) & ABI_LEN_MASK) == 0);
    JT_CHECK(clean());
}

JT_TEST(abi_clock)
{
    setup();
    joist_thread a;
    JT_REQUIRE(joist_thread_create(&k, root, &a) == JOIST_OK);
    k.now = 123456789u;
    JT_CHECK(sys(a, ABI_CLOCK, 0, 0, 0, 0) == ABI_DONE && k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(k.th[a].regs.r[ABI_RDX] == 123456789u && k.th[a].regs.r[ABI_R10] == 1u);
    env.ncpus = 4;
    joist_tick(&k, 1000u);
    JT_CHECK(sys(a, ABI_CLOCK, 0, 0, 0, 0) == ABI_DONE && k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(k.th[a].regs.r[ABI_RDX] == 123457789u && k.th[a].regs.r[ABI_R10] == 4u);
    env.ncpus = 0;
    JT_CHECK(clean());
}

JT_TEST(abi_yield)
{
    setup();
    joist_thread a;
    JT_REQUIRE(joist_thread_create(&k, root, &a) == JOIST_OK);
    uint64_t before = k.th[a].ready_seq;
    JT_CHECK(sys(a, ABI_YIELD, 0, 0, 0, 0) == ABI_DONE && k.th[a].regs.r[ABI_RAX] == JOIST_OK);
    JT_CHECK(k.th[a].ready_seq > before);
    JT_CHECK(clean());
}

