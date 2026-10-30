/* SPDX-License-Identifier: GPL-3.0-only */
/* hello.c - the Phase 2 root task: the first half of the exit criterion
 * ("a userspace hello task runs"). Runs in ring 3, says hello through
 * debug_write, and exits with 0. */
#include "../sys.h"

void _start(void);

void _start(void)
{
    static const char msg[] = "hello from user mode\n";
    if (debug_write(msg, sizeof msg - 1u) != sizeof msg - 1u) {
        sys_exit(1);
    }
    sys_exit(0);
}
