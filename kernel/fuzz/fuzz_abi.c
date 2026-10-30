/* SPDX-License-Identifier: GPL-3.0-only */
/* fuzz_abi.c - libFuzzer over the system-call ABI (Phase 3 step 4; the
 * plan's "fuzz the full system-call surface"). Each input is a sequence of
 * system calls from a few threads, decoded from the input bytes: a real
 * call number most of the time, arguments mostly small (so they name real
 * slots and objects) and now and then anything, random message and
 * capability registers. After every call: every invariant of the core
 * holds, an unknown call number changed no register but RAX, and threads
 * whose blocking call finished get their outputs written, as the kernel
 * does before resuming them. */
#include "abi.h"
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct joist_kernel k;
static struct abi_env env;
static struct vm_space nowhere;

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

struct src {
    const uint8_t *p;
    size_t n;
};

static uint64_t take(struct src *s, unsigned bytes)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i++) {
        v = v << 8 | (s->n != 0 ? (s->n--, *s->p++) : 0u);
    }
    return v;
}

static const uint64_t calls[] = {
    ABI_CALL, ABI_RECV, ABI_REPLY, ABI_REPLY_RECV, ABI_SIGNAL, ABI_WAIT, ABI_RECV_SLOTS,
    ABI_REPLY_SAVE, ABI_REPLY_SAVED, ABI_OBJ_CREATE, ABI_CAP_DERIVE, ABI_CAP_MINT,
    ABI_CAP_DELETE, ABI_CAP_READ, ABI_CAP_GIVE, ABI_CSPACE_GROW, ABI_CSPACE_SHRINK,
    ABI_CAP_COPY_BATCH, ABI_OBJ_KILL,
    ABI_TASK_CREATE, ABI_TASK_DESTROY, ABI_THREAD_SPAWN, ABI_THREAD_KILL, ABI_THREAD_SUSPEND,
    ABI_THREAD_RESUME, ABI_THREAD_ABORT, ABI_THREAD_SET_FAULT, ABI_TASK_SET_FAULT,
    ABI_FRAME_SPLIT, ABI_MAP, ABI_UNMAP, ABI_SC_CREATE, ABI_SC_BIND, ABI_SC_UNBIND,
    ABI_TASK_SET_MCP, ABI_CONTINUE, ABI_TASK_REAP, ABI_TASK_DESCENDANT, ABI_NOSENDERS_NEXT,
    ABI_BIND_NOSENDERS, ABI_REQUEST_DEADNAME};

/* An argument: small 7 times in 8, else anything. */
static uint64_t arg(struct src *s)
{
    uint64_t b = take(s, 1);
    return (b & 7u) != 0 ? b >> 3 : take(s, 8);
}

static int known(uint64_t nr)
{
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++) {
        if (calls[i] == nr) {
            return 1;
        }
    }
    return nr == ABI_DEBUG_WRITE || nr == ABI_EXIT;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct src s = {data, size};
    joist_cspace root;
    joist_init(&k);
    (void)joist_boot(&k, 96, &root);
    memset(&env, 0, sizeof env);
    env.k = &k;
    env.space_of = space_of;
    env.out = out;
    (void)joist_boot_frame(&k, root, 30, 0x100, 64);
    for (int i = 0; i < 3; i++) {
        joist_thread t;
        (void)joist_thread_create(&k, root, &t);
    }
    char why[200];
    for (unsigned step = 0; step < 400u && s.n != 0 && !env.finished; step++) {
        k.preempt = (uint32_t)(take(&s, 1) % 4u); /* half-done destruction too */
        /* A running thread, the first at or after the chosen index. */
        uint32_t t = (uint32_t)(take(&s, 1) % JOIST_MAX_THREADS);
        uint32_t n = 0;
        while (k.th[t].state != JOIST_T_RUNNING && n++ < JOIST_MAX_THREADS) {
            t = (t + 1u) % JOIST_MAX_THREADS;
        }
        if (k.th[t].state != JOIST_T_RUNNING) {
            break; /* nobody left to run */
        }
        abi_complete(&env, t);
        struct joist_regs *r = &k.th[t].regs;
        uint64_t sel = take(&s, 1);
        uint64_t nr = sel < 240u ? calls[sel % (sizeof calls / sizeof calls[0])] : take(&s, 8);
        if (nr == ABI_EXIT || nr == ABI_DEBUG_WRITE) {
            nr = 7; /* exit would end the run; debug_write has its own fuzzer */
        }
        r->r[ABI_RAX] = nr;
        r->r[ABI_RDI] = arg(&s);
        r->r[ABI_RSI] = arg(&s);
        r->r[ABI_RDX] = arg(&s);
        r->r[ABI_R10] = arg(&s);
        r->r[ABI_R8] = arg(&s);
        r->r[ABI_R9] = take(&s, 2) | take(&s, 1) << 16 | take(&s, 2) << 22;
        for (unsigned i = 0; i < 4u; i++) {
            r->r[ABI_R12 + i] = take(&s, 2);
        }
        struct joist_regs before = *r;
        int outcome = abi_syscall(&env, t);
        if (!known(nr)) {
            before.r[ABI_RAX] = (uint64_t)0 - SYS_E_NOSYS;
            if (outcome != ABI_DONE || memcmp(&k.th[t].regs, &before, sizeof before) != 0) {
                fprintf(stderr, "fuzz_abi: unknown call %llu changed registers\n",
                        (unsigned long long)nr);
                abort();
            }
        }
        if (outcome == ABI_DONE && k.th[t].state == JOIST_T_RUNNING &&
            k.th[t].regs.r[ABI_RAX] == (uint64_t)JOIST_BLOCKED) {
            fprintf(stderr, "fuzz_abi: call %llu returned BLOCKED to user\n", (unsigned long long)nr);
            abort();
        }
        if (joist_check_invariants(&k, why, sizeof why) != 0) {
            fprintf(stderr, "fuzz_abi: step %u, call %llu: %s\n", step, (unsigned long long)nr, why);
            abort();
        }
    }
    k.preempt = 0;
    (void)joist_continue(&k, 0);
    if (joist_check_invariants(&k, why, sizeof why) != 0) {
        fprintf(stderr, "fuzz_abi: at the end: %s\n", why);
        abort();
    }
    return 0;
}
