/* SPDX-License-Identifier: GPL-3.0-only */
/* badptr.c - Phase 2 self-test root task: debug_write with a kernel-half
 * pointer must be refused (no output, no kernel fault), then exit 0. */
#include "../sys.h"

void _start(void);

void _start(void)
{
    static const char ok[] = "badptr: refused as expected\n";
    uint64_t r = debug_write((const void *)0xffffffff80000000ull, 16);
    if (r != (uint64_t)0 - 1u) { /* SYS_E_RANGE */
        sys_exit(1);
    }
    debug_write(ok, sizeof ok - 1u);
    sys_exit(0);
}
