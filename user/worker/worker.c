/* worker.c - one program, four parts in the scheduler test (Phase 3 step
 * 5; schedroot loads it as several tasks): what it does is the role its
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
    default:
        fail("no role");
    }
}
