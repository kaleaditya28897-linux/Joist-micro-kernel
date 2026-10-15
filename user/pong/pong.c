/* pong.c - the server half of Phase 3 step 4's ping-pong: receives on the
 * endpoint in slot 0 and answers every message with its first register plus
 * one, forever (the server loop: Recv once, then ReplyRecv), and the IPC
 * buffer's words, if a message brings any, complemented. */
#include "../lib/joist.h"

void _start(void);

static uint64_t buf[512] __attribute__((aligned(4096)));

void _start(void)
{
    struct j_msg m = {0};
    if (j_ipc_buffer(buf) != J_OK) { /* (for ping's words, if it sends any) */
        j_puts("pong: ipc_buffer refused\n");
        j_exit(1);
    }
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
        /* The buffer's words, if any came, go back complemented. */
        unsigned n = J_WORDS(m.caps);
        for (unsigned w = 0; w < n; w++) {
            buf[w] = ~buf[w];
        }
        m.caps = J_WORDS_SET(n);
        if (j_reply_recv(0, &m, J_FOREVER) != J_OK) {
            j_puts("pong: reply_recv failed\n");
            j_exit(3);
        }
    }
}
