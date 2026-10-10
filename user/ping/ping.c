/* ping.c - the client half of Phase 3 step 4's ping-pong: PING_ROUNDS calls
 * on the endpoint in slot 0, each checked to come back plus one, then a
 * report to the root task on the endpoint in slot 1. */
#include "../lib/joist.h"

#ifndef PING_ROUNDS
#define PING_ROUNDS 1000u
#endif

void _start(void);

void _start(void)
{
    struct j_msg m = {0};
    for (uint64_t i = 0; i < PING_ROUNDS; i++) {
        m.r[0] = i;
        m.r[1] = ~i;
        m.caps = 0;
        uint64_t e = j_call(0, &m, J_FOREVER);
        if (e != J_OK || m.r[0] != i + 1u || m.r[1] != ~i) {
            j_puts("ping: FAILED at round ");
            j_putu(i);
            j_puts("\n");
            j_exit(1);
        }
    }
    j_puts("ping: ");
    j_putu(PING_ROUNDS);
    j_puts(" round trips ok\n");
    m.r[0] = PING_ROUNDS;
    m.caps = 0;
    (void)j_call(1, &m, J_FOREVER); /* tell the root task */
    j_exit(0);
}
