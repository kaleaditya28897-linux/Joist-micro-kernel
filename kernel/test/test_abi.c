/* test_abi.c - the system-call ABI (Phase 3 step 4; sys/abi.h): threads
 * driven through their saved registers only, as the kernel drives them. */
#include "abi.h"
#include "harness.h"
#include "internal.h"

#include <string.h>

static struct joist_kernel k;
static struct abi_env env;
static joist_cspace root;
static struct vm_space nowhere; /* debug_write's space: nothing mapped */

static const struct vm_space *space_of(void *ctx, uint32_t s)
{
    (void)ctx;
    (void)s;
    return &nowhere;
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
