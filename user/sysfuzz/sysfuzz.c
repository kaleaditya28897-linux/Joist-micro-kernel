/* sysfuzz.c - the Phase 2 system-call fuzzer (exit criterion, part 2: "the
 * kernel survives fuzzing of its tiny syscall surface").
 *
 * A root task making SYSFUZZ_CALLS system calls from a seeded generator:
 * random call numbers (never exit, until the end), pointers from a set of
 * interesting classes (null, its own code and data, its stack, the kernel
 * image, the direct map, non-canonical, the top of the user half, random),
 * random lengths (0, small, page-crossing, past the limit, huge), and every
 * other general register random. Each call must come back, with every
 * register but rax (the result), rcx and r11 (clobbered by `syscall`)
 * unchanged, and with a result that is one of the defined outcomes, exactly
 * the predicted one where the arguments make it certain. */
#include "../sys.h"

#ifndef SYSFUZZ_SEED
#define SYSFUZZ_SEED 0x5eedf00dull
#endif
#ifndef SYSFUZZ_CALLS
#define SYSFUZZ_CALLS 1000000u
#endif

struct regs {
    uint64_t rax, rbx, rdx, rsi, rdi, rbp, r8, r9, r10, r12, r13, r14, r15;
};

void fuzz_call(const struct regs *in, struct regs *out);
void _start(void);

static uint64_t rs = SYSFUZZ_SEED;

static uint64_t rnd(void)
{
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return rs * 0x2545F4914F6CDD1DULL;
}

static char dots[64]; /* the only memory valid writes may print: '.' */

static void say(const char *s)
{
    uint64_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    debug_write(s, n);
}

static void say_u(uint64_t v)
{
    char b[24];
    int i = 23;
    b[i] = '\0';
    do {
        b[--i] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0 && i > 0);
    say(&b[i]);
}

#define E_RANGE ((uint64_t)0 - 1u)
#define E_INVAL ((uint64_t)0 - 2u)
#define E_NOSYS ((uint64_t)0 - 3u)

/* Pointer classes. Kernel-half and non-canonical ones are certainly refused
 * (certain_bad). Any class that may be valid gets small lengths (printable),
 * so valid writes print a few bytes, not pages of code: large valid copies
 * are covered byte-exact by the host tests. */
static uint64_t pick_ptr(int *certain_bad, int *printable)
{
    *certain_bad = 0;
    *printable = 0;
    switch (rnd() % 10u) {
    case 0:
        return 0;
    case 1: /* its own code */
        *printable = 1;
        return (uint64_t)(uintptr_t)_start + rnd() % 64u;
    case 2:
        *printable = 1;
        return (uint64_t)(uintptr_t)dots + rnd() % 32u;
    case 3: /* its stack: just below 0x400000 */
        *printable = 1;
        return 0x3f8000u + rnd() % 0x8000u;
    case 4:
        *certain_bad = 1;
        return 0xffffffff80000000ull + rnd() % 0x10000u; /* the kernel image */
    case 5:
        *certain_bad = 1;
        return 0xffff800000000000ull + rnd() % 0x10000000u; /* the direct map */
    case 6:
        *certain_bad = 1;
        return 0x0000800000000000ull + rnd() % 0x1000u; /* non-canonical */
    case 7:
        *printable = 1; /* unmapped today, but keep it small if that changes */
        return 0x00007ffffffff000ull + rnd() % 0x1000u; /* the top of the user half */
    case 8:
        *printable = 1;
        return rnd() % 0x400000u; /* low, mostly unmapped */
    default:
        return rnd();
    }
}

static uint64_t pick_len(int printable)
{
    if (printable) {
        return rnd() % 3u; /* keep the serial log readable */
    }
    switch (rnd() % 6u) {
    case 0:
        return 0;
    case 1:
        return 1u + rnd() % 64u;
    case 2:
        return 4000u + rnd() % 200u; /* crosses a page */
    case 3:
        return 4097u + rnd() % 100u; /* past DEBUG_WRITE_MAX */
    case 4:
        return 0xffffffffffffffffull - rnd() % 16u;
    default:
        return rnd();
    }
}

void _start(void)
{
    for (unsigned i = 0; i < sizeof dots; i++) {
        dots[i] = '.';
    }
    say("sysfuzz: seed ");
    say_u(SYSFUZZ_SEED);
    say(", ");
    say_u(SYSFUZZ_CALLS);
    say(" calls\n");
    uint64_t writes_ok = 0, refused = 0, nosys = 0;
    for (uint64_t n = 0; n < SYSFUZZ_CALLS; n++) {
        struct regs in;
        struct regs out;
        uint64_t *w = (uint64_t *)&in;
        for (unsigned i = 0; i < sizeof in / sizeof in.rax; i++) {
            w[i] = rnd();
        }
        int bad = 0;
        int printable = 0;
        in.rax = (rnd() % 4u == 0) ? SYS_DEBUG_WRITE : rnd() % 8u == 0 ? rnd() : SYS_DEBUG_WRITE;
        if (in.rax == SYS_EXIT) {
            in.rax = 7; /* never exit early: that would end the run */
        }
        if (in.rax == SYS_DEBUG_WRITE) {
            in.rdi = pick_ptr(&bad, &printable);
            in.rsi = pick_len(printable);
        }
        fuzz_call(&in, &out);
        /* Every register but rax is the caller's again. */
        const uint64_t *a = (const uint64_t *)&in;
        const uint64_t *b = (const uint64_t *)&out;
        for (unsigned i = 1; i < sizeof in / sizeof in.rax; i++) {
            if (a[i] != b[i]) {
                say("sysfuzz: FAILED: register ");
                say_u(i);
                say(" changed on call ");
                say_u(n);
                say("\n");
                sys_exit(2);
            }
        }
        uint64_t r = out.rax;
        int ok;
        if (in.rax != SYS_DEBUG_WRITE) {
            ok = r == E_NOSYS;
            nosys++;
        } else if (in.rsi > 4096u) {
            ok = r == E_INVAL;
            refused++;
        } else if (in.rsi == 0) {
            ok = r == 0;
        } else if (bad) {
            ok = r == E_RANGE;
            refused++;
        } else {
            ok = r == in.rsi || r == E_RANGE;
            if (r == in.rsi) {
                writes_ok++;
            } else {
                refused++;
            }
        }
        if (!ok) {
            say("sysfuzz: FAILED: unexpected result on call ");
            say_u(n);
            say("\n");
            sys_exit(3);
        }
    }
    say("\nsysfuzz: done: ");
    say_u(writes_ok);
    say(" writes, ");
    say_u(refused);
    say(" refused, ");
    say_u(nosys);
    say(" unknown calls; every call returned, registers preserved\n");
    sys_exit(0);
}
