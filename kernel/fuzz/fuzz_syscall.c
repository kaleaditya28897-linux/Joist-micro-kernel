/* fuzz_syscall.c - libFuzzer on Phase 2's system-call dispatcher (step 6;
 * §8.1): any call number and arguments against a simulated task; the result
 * must match the world (test/sys_world.h), with no output on failure. */
#include "sys_world.h"

#include <stdio.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static struct vm_space ws;
static int ready;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint64_t v[3] = {0, 0, 0};
    if (!ready) {
        ws = sw_space();
        ready = 1;
    }
    memcpy(v, data, size < sizeof v ? size : sizeof v);
    sw_outlen = 0;
    sw_calls = 0;
    struct sys_env e = {.task = &ws, .out = sw_sink, .ctx = NULL};
    uint64_t r = sys_dispatch(&e, v[0], v[1], v[2]);
    if (!sw_consistent(v[0], v[1], v[2], r, &e)) {
        fprintf(stderr, "fuzz_syscall: nr %llx a0 %llx a1 %llx -> %llx inconsistent\n",
                (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
                (unsigned long long)r);
        abort();
    }
    return 0;
}
