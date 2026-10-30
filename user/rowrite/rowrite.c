/* SPDX-License-Identifier: GPL-3.0-only */
/* rowrite.c - Phase 2 self-test root task: a write to its own read-only
 * data must be a user page fault at that address. */
void _start(void);

static const char ro[] = "read-only";

void _start(void)
{
    *(volatile char *)(unsigned long)ro = 'x';
    for (;;) {
    }
}
