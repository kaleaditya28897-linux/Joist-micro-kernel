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
    default:
        break;
    }
    w->count = 2;
    j_exit(0); /* from a task that isn't the root: this thread ends */
}
