/* fuzzchild.c - makes system calls at random, every call the ABI has and
 * numbers it hasn't, with random arguments (Phase 3's exit: "fuzzes the
 * full system-call surface"). growroot starts it in a task of its own, with
 * a few capabilities, lets it run, and destroys it: whatever it did must
 * leave nothing behind in the kernel. RDI is the seed.
 *
 * Unlike user/sysfuzz (Phase 2), which is the root task and must survive,
 * this one may do anything to itself: kill its own thread, destroy its own
 * task, wait for ever. Its root takes it away in any case. */
#include "../lib/joist.h"

void _start(uint64_t seed);

static uint64_t rng;
static uint64_t next(void)
{
    rng ^= rng >> 12;
    rng ^= rng << 25;
    rng ^= rng >> 27;
    return rng * 0x2545F4914F6CDD1DULL;
}

/* Every call number the ABI has (docs/abi.md, *Encoding*), and a few it
 * hasn't. Exit (2) is left out: it would end the thread at the first try. */
static const uint8_t calls[] = {1, 3, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21, 22, 23, 24,
                                25, 26, 27, 30, 31, 32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 50,
                                51, 52, 53, 54, 60, 61, 62, 63, 64, 65, 70, 71, 72, 0, 4, 9, 19, 99};

static uint8_t scratch[256];

/* An argument: mostly a small slot number (where its capabilities are), now
 * and then a pointer into its own memory, or anything at all. */
static uint64_t arg(void)
{
    uint64_t r = next();
    switch (r % 8u) {
    case 0:
        return next();
    case 1:
        return (uint64_t)(uintptr_t)scratch + (next() % sizeof scratch);
    case 2:
        return (next() % 4u) * J_MS / 4u; /* a short timeout */
    default:
        return r >> 60; /* 0..15 */
    }
}

void _start(uint64_t seed)
{
    rng = seed * 0x9E3779B97F4A7C15ULL + 1u;
    for (;;) {
        struct j_regs r = {.rax = calls[next() % sizeof calls],
                           .rdi = arg(), .rsi = arg(), .rdx = arg(), .r10 = arg(), .r8 = arg(),
                           .r9 = next() % 3u == 0 ? next() : 0,
                           .r12 = next(), .r13 = next(), .r14 = next(), .r15 = next()};
        if (r.rax == J_SYS_DEBUG_WRITE) {
            r.rsi = 0; /* (its pointer is still random: the copy's checks run; nothing is printed) */
        }
        if ((r.rax == J_SYS_CALL || r.rax == J_SYS_RECV || r.rax == J_SYS_WAIT ||
             r.rax == J_SYS_REPLY_RECV) && next() % 4u != 0) {
            r.rsi = next() % J_MS; /* mostly don't wait long */
        }
        j_syscall(&r);
    }
}
