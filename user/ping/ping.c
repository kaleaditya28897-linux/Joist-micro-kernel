/* ping.c - the client half of Phase 3 step 4's ping-pong: PING_ROUNDS calls
 * on the endpoint in slot 0, each checked to come back plus one, then a
 * report to the root task on the endpoint in slot 1. With PING_WORDS, each
 * call also carries that many words of its IPC buffer, which must come
 * back complemented. */
#include "../lib/joist.h"

#ifndef PING_ROUNDS
#define PING_ROUNDS 1000u
#endif
/* Words of the IPC buffer to send each round (Phase 3's exit), checked on
 * the way back: pong returns each word complemented. 0: registers only. */
#ifndef PING_WORDS
#define PING_WORDS 0u
#endif

static uint64_t buf[512] __attribute__((aligned(4096)));

void _start(void);

void _start(void)
{
    struct j_msg m = {0};
    if (PING_WORDS != 0 && j_ipc_buffer(buf) != J_OK) {
        j_puts("ping: ipc_buffer refused\n");
        j_exit(1);
    }
    for (uint64_t i = 0; i < PING_ROUNDS; i++) {
        m.r[0] = i;
        m.r[1] = ~i;
        m.caps = J_WORDS_SET(PING_WORDS);
        for (unsigned w = 0; w < PING_WORDS; w++) {
            buf[w] = i * 1000003u + w;
        }
        uint64_t e = j_call(0, &m, J_FOREVER);
        int words_ok = J_WORDS(m.caps) == PING_WORDS && !J_LOST(m.caps);
        for (unsigned w = 0; w < PING_WORDS && words_ok; w++) {
            words_ok = buf[w] == ~(i * 1000003u + w);
        }
        if (e != J_OK || m.r[0] != i + 1u || m.r[1] != ~i || !words_ok) {
            j_puts("ping: FAILED at round ");
            j_putu(i);
            j_puts("\n");
            j_exit(1);
        }
    }
    j_puts("ping: ");
    j_putu(PING_ROUNDS);
    j_puts(" round trips ok");
    if (PING_WORDS != 0) {
        j_puts(", ");
        j_putu(PING_WORDS);
        j_puts(" words of the IPC buffer each way each time");
    }
    j_puts("\n");
    m.r[0] = PING_ROUNDS;
    m.caps = 0;
    (void)j_call(1, &m, J_FOREVER); /* tell the root task */
    j_exit(0);
}
