/* spin.c - Phase 2 self-test root task: busy for about a second, so the
 * timer interrupts it in ring 3 many times (TSS stack switch, iretq back to
 * user mode), then exit 0. */
#include "../sys.h"

void _start(void);

void _start(void)
{
    static const char done[] = "spin: done\n";
    for (volatile uint64_t i = 0; i < 400000000u; i++) {
    }
    debug_write(done, sizeof done - 1u);
    sys_exit(0);
}
