/* SPDX-License-Identifier: GPL-3.0-only */
/* test_syscall.c - Phase 2's debug system calls (step 6): unit tests and a
 * property test over random arguments; fuzz/fuzz_syscall.c does the same
 * from libFuzzer. */
#include "harness.h"
#include "sys_world.h"

static struct vm_space ws;

static struct sys_env env(void)
{
    sw_outlen = 0;
    sw_calls = 0;
    return (struct sys_env){.task = &ws, .out = sw_sink, .ctx = NULL};
}

JT_TEST(sys_debug_write_and_exit)
{
    ws = sw_space();
    struct sys_env e = env();
    JT_CHECK(sys_dispatch(&e, SYS_DEBUG_WRITE, SW_UBASE + 4000, 200) == 200); /* pages 0..1 */
    JT_CHECK(sw_calls == 1 && sw_outlen == 200 &&
             sw_consistent(SYS_DEBUG_WRITE, SW_UBASE + 4000, 200, 200, &e));
    e = env();
    JT_CHECK(sys_dispatch(&e, SYS_DEBUG_WRITE, 0, 0) == 0); /* empty: fine, writes nothing */
    e = env();
    JT_CHECK(sys_dispatch(&e, SYS_EXIT, 42, 0) == 0 && e.exited && e.exit_code == 42);
}

JT_TEST(sys_refusals_write_nothing)
{
    ws = sw_space();
    struct sys_env e = env();
    JT_CHECK(sys_dispatch(&e, SYS_DEBUG_WRITE, SW_UBASE + 3 * 4096u, 1) ==
             (uint64_t)0 - SYS_E_RANGE);
    JT_CHECK(sys_dispatch(&e, SYS_DEBUG_WRITE, SW_UBASE + 2 * 4096u + 4000, 200) ==
             (uint64_t)0 - SYS_E_RANGE);
    JT_CHECK(sys_dispatch(&e, SYS_DEBUG_WRITE, 0xffffffff80000000ull, 16) ==
             (uint64_t)0 - SYS_E_RANGE);
    JT_CHECK(sys_dispatch(&e, SYS_DEBUG_WRITE, SW_UBASE, DEBUG_WRITE_MAX + 1u) ==
             (uint64_t)0 - SYS_E_INVAL);
    JT_CHECK(sys_dispatch(&e, 0, 0, 0) == (uint64_t)0 - SYS_E_NOSYS);
    JT_CHECK(sys_dispatch(&e, 0xffffffffffffffffull, 0, 0) == (uint64_t)0 - SYS_E_NOSYS);
    JT_CHECK(sw_calls == 0 && !e.exited); /* nothing written, nothing exited */
}

JT_TEST(props_syscall_random_arguments)
{
    ws = sw_space();
    uint64_t r = 0xfeed;
    unsigned long writes = 0, refused = 0;
    for (int i = 0; i < 200000; i++) {
        r ^= r >> 12;
        r ^= r << 25;
        r ^= r >> 27;
        uint64_t x = r * 0x2545F4914F6CDD1DULL;
        uint64_t nr = x % 5u;
        uint64_t a0 = (x >> 8) % 3u == 0 ? x : SW_UBASE - 4096u + (x >> 16) % (10u * 4096u);
        uint64_t a1 = (x >> 4) % 4u == 0 ? x >> 3 : (x >> 24) % 9000u;
        struct sys_env e = env();
        uint64_t res = sys_dispatch(&e, nr, a0, a1);
        if (!sw_consistent(nr, a0, a1, res, &e)) {
            fprintf(stderr, "  iteration %d: nr %llu a0 %llx a1 %llx -> %llx\n", i,
                    (unsigned long long)nr, (unsigned long long)a0, (unsigned long long)a1,
                    (unsigned long long)res);
            JT_CHECK(0);
            return;
        }
        if (nr == SYS_DEBUG_WRITE) {
            if (sw_calls != 0) {
                writes++;
            } else {
                refused++;
            }
        }
    }
    fprintf(stderr, "  coverage debug_write written=%lu refused=%lu\n", writes, refused);
    JT_CHECK(writes > 1000 && refused > 1000);
}
