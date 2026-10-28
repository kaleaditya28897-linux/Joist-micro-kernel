/* faulter.c - one program that misbehaves in the way its root asks (Phase 3
 * step 6; faultroot loads it as several tasks, lib/work.h says how): the
 * faults go to whoever its root named as handler. */
#include "../lib/joist.h"
#include "../lib/work.h"

void _start(uint64_t a, uint64_t b);

void _start(uint64_t a, uint64_t b)
{
    struct work *w = (struct work *)(uintptr_t)WORK_VA;
    struct j_msg m = {0};
    volatile uint64_t zero = 0;
    uint64_t v;
    if (w->role == WORK_SPIN) { /* no fault: something to suspend and resume */
        for (;;) {
            work_spin(w, 1u << 20);
        }
    }
    w->count = 1;
    switch (w->role) {
    case FAULT_UD:
        __asm__ volatile("ud2");
        break;
    case FAULT_PAGE:
        *(volatile uint64_t *)(uintptr_t)FAULT_VA = w->arg;
        w->arg = *(volatile uint64_t *)(uintptr_t)FAULT_VA + 1u;
        break;
    case FAULT_PAGE2:
        *(volatile uint64_t *)(uintptr_t)FAULT_VA = 1;
        while (w->aux == 0) {
        }
        *(volatile uint64_t *)(uintptr_t)FAULT_VA = 2;
        break;
    case FAULT_GP:
        __asm__ volatile("hlt");
        break;
    case FAULT_DIV:
        w->arg = 100u / zero;
        break;
    case FAULT_TLS:
        __asm__ volatile("movq %%fs:0, %0" : "=r"(v));
        w->arg = v;
        break;
    case FAULT_BLOCK:
        w->arg = j_recv(0, &m, J_FOREVER);
        break;
    case FAULT_REGS:
        w->arg = a << 32 | b;
        break;
    case FAULT_BENCH: {
        /* Each touch faults; the handler maps the page and replies: the
         * time from just before the touch to just after is one round trip
         * through the kernel, the handler and back. */
        static uint64_t d[FAULT_BENCH_MAX];
        uint64_t n = w->arg < FAULT_BENCH_MAX ? w->arg : FAULT_BENCH_MAX;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t t0 = work_tsc();
            *(volatile uint64_t *)(uintptr_t)(FAULT_BENCH_VA + i * J_PAGE) = i;
            d[i] = work_tsc() - t0;
        }
        for (uint64_t i = 1; i < n; i++) { /* insertion sort: n is small */
            uint64_t x = d[i];
            uint64_t j = i;
            for (; j > 0 && d[j - 1u] > x; j--) {
                d[j] = d[j - 1u];
            }
            d[j] = x;
        }
        w->aux = n != 0 ? d[n / 2u] : 0;
        w->arg = n != 0 ? d[0] : 0;
        break;
    }
    default:
        break;
    }
    w->count = 2;
    j_exit(0); /* from a task that isn't the root: this thread ends */
}
