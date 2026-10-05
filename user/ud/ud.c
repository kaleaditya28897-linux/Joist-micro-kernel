/* ud.c - Phase 2 self-test root task: an invalid instruction in ring 3 must
 * be reported as the root task's fault, not a kernel exception. */
void _start(void);

void _start(void)
{
    __asm__ volatile("ud2");
    for (;;) {
    }
}
