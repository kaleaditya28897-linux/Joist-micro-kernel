/* SPDX-License-Identifier: GPL-3.0-only */
/* worker.c - one program, six parts in the scheduler test (Phase 3 step
 * 5, and the FPU's: Phase 5 step 7; schedroot loads it as several tasks): what it does is the role its
 * root wrote in the page they share (lib/work.h). */
#include "../lib/joist.h"
#include "../lib/work.h"

void _start(void);

static __attribute__((noreturn)) void fail(const char *why)
{
    j_puts("worker: ");
    j_puts(why);
    j_puts("\n");
    j_exit(1);
}

void _start(void)
{
    struct work *w = (struct work *)(uintptr_t)WORK_VA;
    struct j_msg m = {0};
    switch (w->role) {
    case WORK_SPIN:
        for (;;) {
            work_spin(w, 1u << 20);
        }
    case WORK_SERVE:
        if (j_recv(0, &m, J_FOREVER) != J_OK) {
            fail("recv failed");
        }
        for (;;) {
            work_spin(w, w->arg);
            m.caps = 0;
            if (j_reply_recv(0, &m, J_FOREVER) != J_OK) {
                fail("reply_recv failed");
            }
        }
    case WORK_CLIENT:
        for (;;) {
            m.caps = 0;
            if (j_call(0, &m, J_FOREVER) != J_OK) {
                fail("call failed");
            }
            w->count = w->count + 1u;
        }
    case WORK_CALLS:
        for (;;) {
            (void)j_frame_pages(1); /* an empty slot: refused at once */
            w->count = w->count + 1u;
        }
    case WORK_TICK: {
        uint64_t last = work_tsc();
        for (;;) {
            if (j_recv(0, &m, w->arg) != J_E_TIMEOUT) {
                fail("a sleep didn't time out");
            }
            uint64_t now = work_tsc();
            if (now - last > w->aux) {
                w->aux = now - last;
            }
            last = now;
            w->count = w->count + 1u;
        }
    }
    case WORK_FPU: {
        /* By hand, in assembly: the program is built with no FPU or SSE
         * code of its own (-mgeneral-regs-only), so only these touch them. */
        uint32_t mx = 0;
        uint16_t fcw = 0;
        __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(mx), "=m"(fcw));
        if (mx != 0x1f80u || fcw != 0x37fu) {
            w->aux = 2;
        }
        uint32_t mine = 0x1f80u | (uint32_t)(w->arg & 3u) << 13; /* MXCSR.RC */
        uint64_t v = w->arg;
        __asm__ volatile("ldmxcsr %0\n\tmovq %1, %%xmm0\n\tmovq %1, %%xmm15" : : "m"(mine), "r"(v));
        for (;;) {
            work_spin(w, 1u << 12);
            uint64_t x0 = 0;
            uint64_t x15 = 0;
            __asm__ volatile("stmxcsr %0\n\tmovq %%xmm0, %1\n\tmovq %%xmm15, %2"
                             : "=m"(mx), "=r"(x0), "=r"(x15));
            if (x0 != v || x15 != v || mx != mine) {
                w->aux = 1;
            }
        }
    }
    default:
        fail("no role");
    }
}
