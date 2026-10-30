/* SPDX-License-Identifier: GPL-3.0-only */
/* sys_world.h - a simulated task for the system-call tests and fuzzer:
 * 8 user pages at 0x400000 (page 3 kernel-only, page 6 unmapped), each byte
 * (page * 7 + offset) & 0xff, over simulated physical memory. */
#ifndef JOIST_TEST_SYS_WORLD_H
#define JOIST_TEST_SYS_WORLD_H

#include "syscall.h"

#include <stdlib.h>
#include <string.h>

#define SW_FRAMES 64u
#define SW_PBASE  0x300000ull
#define SW_UBASE  0x400000ull
static uint8_t sw_mem[SW_FRAMES * 4096u] __attribute__((aligned(4096)));
static uint32_t sw_next;

static kerr sw_frame(void *ctx, uint64_t *pa)
{
    (void)ctx;
    if (sw_next == SW_FRAMES) {
        return KERR_NOMEM;
    }
    memset(&sw_mem[sw_next * 4096u], 0, 4096u);
    *pa = SW_PBASE + (uint64_t)sw_next++ * 4096u;
    return KERR_OK;
}

static void *sw_ptr(void *ctx, uint64_t pa)
{
    (void)ctx;
    if (pa < SW_PBASE || pa - SW_PBASE >= sizeof sw_mem) {
        abort();
    }
    return &sw_mem[pa - SW_PBASE];
}

static int sw_valid_page(uint64_t i)
{
    return i < 8u && i != 3u && i != 6u;
}

static struct vm_space sw_space(void)
{
    struct vm_space s;
    sw_next = 0;
    if (vm_create(&s, sw_frame, sw_ptr, NULL) != KERR_OK) {
        abort();
    }
    for (unsigned i = 0; i < 8u; i++) {
        if (i == 6u) {
            continue;
        }
        uint64_t pa;
        if (sw_frame(NULL, &pa) != KERR_OK) {
            abort();
        }
        uint8_t *p = sw_ptr(NULL, pa);
        for (unsigned b = 0; b < 4096u; b++) {
            p[b] = (uint8_t)(i * 7u + b);
        }
        if (vm_map(&s, SW_UBASE + i * 4096u, pa, 4096u, VM_PAGE, i == 3u ? VM_W : VM_W | VM_USER) !=
            KERR_OK) {
            abort();
        }
    }
    return s;
}

/* Output capture. */
static uint8_t sw_out[8192];
static size_t sw_outlen;
static unsigned sw_calls;

static void sw_sink(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    sw_calls++;
    if (n <= sizeof sw_out - sw_outlen) {
        memcpy(sw_out + sw_outlen, buf, n);
        sw_outlen += n;
    }
}

/* Is the result of one call consistent with the world? */
static int sw_consistent(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t r, const struct sys_env *e)
{
    if (nr == SYS_DEBUG_WRITE) {
        int ok = a1 <= DEBUG_WRITE_MAX;
        for (uint64_t a = a0; ok && a1 != 0 && a < a0 + a1; a = (a | 4095u) + 1u) {
            ok = a >= SW_UBASE && a < SW_UBASE + 8u * 4096u && a0 + a1 > a0 &&
                 sw_valid_page((a - SW_UBASE) / 4096u);
        }
        if (a1 != 0 && a0 > UINT64_MAX - a1) {
            ok = 0;
        }
        if (!ok) {
            return r == (uint64_t)0 - (a1 > DEBUG_WRITE_MAX ? SYS_E_INVAL : SYS_E_RANGE) &&
                   sw_calls == 0;
        }
        if (r != a1 || sw_calls != 1 || sw_outlen != a1) {
            return 0;
        }
        for (uint64_t k = 0; k < a1; k++) {
            uint64_t v = a0 + k - SW_UBASE;
            if (sw_out[k] != (uint8_t)((v / 4096u) * 7u + v % 4096u)) {
                return 0;
            }
        }
        return 1;
    }
    if (nr == SYS_EXIT) {
        return r == 0 && e->exited && e->exit_code == a0 && sw_calls == 0;
    }
    return r == (uint64_t)0 - SYS_E_NOSYS && sw_calls == 0 && !e->exited;
}

#endif /* JOIST_TEST_SYS_WORLD_H */
