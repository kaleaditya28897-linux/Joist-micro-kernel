/* pong.c - the server half of Phase 3 step 4's ping-pong: receives on the
 * endpoint in slot 0 and answers every message with its first register plus
 * one, forever (the server loop: Recv once, then ReplyRecv). */
#include "../lib/joist.h"

void _start(void);

void _start(void)
{
    struct j_msg m = {0};
    if (j_recv(0, &m, J_FOREVER) != J_OK) {
        j_puts("pong: recv failed\n");
        j_exit(1);
    }
    for (;;) {
        if (m.badge != 7) { /* the badge root gave ping's capability */
            j_puts("pong: a message with the wrong badge\n");
            j_exit(2);
        }
        m.r[0] += 1;
        m.caps = 0;
        if (j_reply_recv(0, &m, J_FOREVER) != J_OK) {
            j_puts("pong: reply_recv failed\n");
            j_exit(3);
        }
    }
}
