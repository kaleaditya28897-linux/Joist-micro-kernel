/* SPDX-License-Identifier: GPL-3.0-only */
/* glibctest.c - the first glibc program on Joist (Phase 5 step 7, the
 * design's item 11 d): a static program linked with glibc's sysdeps/joist,
 * started by user/glibcroot as the Hurd's exec server would start it.
 *
 * It uses glibc as any program does and checks what it gets: argv and the
 * environment from start-up, malloc and friends (over mm), snprintf,
 * clock_gettime, nanosleep, sysconf's CPU count, thread-local variables,
 * fork's refusal, and pthreads (htl over Joist threads): four threads
 * contending on one mutex and meeting at a condition variable, each
 * writing to stdout; stdout, the root's console; given a stdin, one read
 * of 200 KiB through the write window (decision 0032). Its checks report
 * through the kernel's debug write, which needs nothing of glibc: each
 * prints "glibc: <what> ok", and the last "glibc: all ok". Then it returns from main, so exit gives the root
 * its status and ends the task.
 *
 * Built by tools/phase5/glibc-test.sh with the cross-built glibc. */
#include <errno.h>
#include <mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Joist's debug write (call 1: the bytes, their number). */
static void say(const char *s)
{
    uint64_t rax = 1, rdi = (uint64_t)(uintptr_t)s, rsi = strlen(s);
    __asm__ volatile("syscall"
                     : "+a"(rax), "+D"(rdi), "+S"(rsi)
                     :
                     : "rcx", "r11", "rdx", "r8", "r9", "r10", "r12", "r13", "r14", "r15", "memory");
}

static int failed;

static void check(int ok, const char *what)
{
    char line[160];
    (void)snprintf(line, sizeof line, "glibc: %s %s\n", what, ok ? "ok" : "FAILED");
    say(line);
    failed |= !ok;
}

static __thread int tls_value = 7;

#define THREADS 4
#define ROUNDS 20000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t met = PTHREAD_COND_INITIALIZER;
static long counter;
static int arrived, tls_bad, write_bad;

/* A robust mutex main holds: another thread's trylock must find it taken
 * (htl writes the owner's pid in the lock word: 0 let it through). */
static pthread_mutex_t robust;

static void *try_robust(void *arg)
{
    (void)arg;
    return (void *)(intptr_t)pthread_mutex_trylock(&robust);
}

static void *worker(void *arg)
{
    int me = (int)(intptr_t)arg;
    tls_value = 100 + me; /* each thread its own */
    /* An RPC from a thread of its own: io_write through its own IPC
     * buffer (each thread's, made on its first call). */
    char line[64];
    int n = snprintf(line, sizeof line, "glibc: thread %d writes to the console\n", me);
    if (write(1, line, (size_t)n) != n) {
        write_bad = 1;
    }
    for (int i = 0; i < ROUNDS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    pthread_mutex_lock(&lock);
    if (tls_value != 100 + me) {
        tls_bad = 1;
    }
    if (++arrived == THREADS) {
        pthread_cond_broadcast(&met);
    }
    while (arrived < THREADS) {
        pthread_cond_wait(&met, &lock);
    }
    pthread_mutex_unlock(&lock);
    return (void *)(intptr_t)(me * 3);
}

/* Byte i of the stdin the selftest gives (kernel/test/selftest.sh's
 * glibcstdin): the top byte of i times Knuth's multiplicative constant. */
static unsigned char stdin_byte(uint32_t i)
{
    return (unsigned char)((uint32_t)(i * 2654435761u) >> 24);
}

/* libc's window region (glibc/sysdeps/joist/joist-mig.c): a reservation
 * of 64 MiB, read and write, that vm_region shows and that vm_deallocate
 * and vm_protect refuse, wholly or in part. */
static int window_region_held(void)
{
    vm_address_t a = 0;
    vm_size_t size = 0;
    for (int i = 0; i < 1024; i++, a += size) {
        vm_prot_t prot, maxp;
        vm_inherit_t inh;
        boolean_t shared;
        mach_port_t obj = MACH_PORT_NULL;
        vm_offset_t off;
        if (vm_region(mach_task_self(), &a, &size, &prot, &maxp, &inh, &shared, &obj, &off) !=
            KERN_SUCCESS) {
            return 0;
        }
        if (size == 64u << 20 && prot == (VM_PROT_READ | VM_PROT_WRITE)) {
            return vm_deallocate(mach_task_self(), a, size) != KERN_SUCCESS &&
                   vm_deallocate(mach_task_self(), a + size - 4096, 8192) != KERN_SUCCESS &&
                   vm_protect(mach_task_self(), a, 4096, 0, VM_PROT_READ) != KERN_SUCCESS;
        }
    }
    return 0;
}

/* "glibctest iobench" (Phase 5 step 10, the plan's measurement): io_read
 * of 2 KiB to 1 MiB from offset 0 of stdin (pread: glibcroot's console
 * serves it from module 3), 200 times each after 20 to warm, timed with
 * the TSC around each call; 4 KiB and less in line through the IPC buffer,
 * more through the write window (decision 0032). The median and the least
 * and greatest, in TSC counts. Warm: the same bytes each time. */
static uint64_t tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (uint64_t)hi << 32 | lo;
}
static int by_value(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static int iobench(void)
{
    static const size_t sizes[] = {2048, 4096, 16384, 65536, 262144, 1048576};
    static uint64_t t[200];
    char *buf = malloc(1u << 20);
    if (buf == NULL) {
        say("iobench: no buffer\n");
        return 1;
    }
    for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        size_t n = sizes[s];
        int bad = 0;
        for (int i = 0; i < 20; i++) {
            bad |= pread(0, buf, n, 0) != (ssize_t)n;
        }
        for (int i = 0; i < 200; i++) {
            uint64_t t0 = tsc();
            ssize_t got = pread(0, buf, n, 0);
            t[i] = tsc() - t0;
            bad |= got != (ssize_t)n;
        }
        qsort(t, 200, sizeof t[0], by_value);
        char line[160];
        (void)snprintf(line, sizeof line, "iobench: io_read of %zu bytes: median %llu, least %llu, most %llu%s\n", n,
                       (unsigned long long)t[100], (unsigned long long)t[0], (unsigned long long)t[199],
                       bad ? " (SHORT READS)" : "");
        say(line);
        if (bad) {
            return 1;
        }
    }
    say("iobench: done\n");
    return 0;
}

/* malloc's main arena in a region (Phase 5 step 11; the owner's choices of
 * 2026-11-18: brk.c and joist-mig.c's break): 200 000 random mallocs and
 * frees of 1 B to 4 KiB (below the mmap threshold, so the main arena's), at
 * most 128 alive, each block filled and checked before it is freed. The
 * break must have grown past 128 KiB, and every block lain in the break's
 * region: from its start (on a page-table boundary, 2 MiB, at or below the
 * break where main began: joist-mig.c's __joist_brk_hold) to the break.
 * Not from where the break was before the test: the heap below it, freed
 * by the test before, is used again. */
static char *brk_at_main;

static int malloc_in_region(void)
{
    enum { LIVE = 128 };
    static unsigned char *blk[LIVE];
    static size_t len[LIVE];
    char *b0 = sbrk(0);
    char *lo = (char *)((uintptr_t)brk_at_main & ~(uintptr_t)0x1fffff);
    uint64_t r = 0x9e3779b97f4a7c15ull;
    int ok = b0 != (char *)-1 && b0 != NULL && brk_at_main != (char *)-1 && brk_at_main != NULL;
    unsigned op_at = 0;
    for (unsigned op = 0; ok && op < 200000u; op++) {
        op_at = op;
        r ^= r << 13;
        r ^= r >> 7;
        r ^= r << 17;
        unsigned i = (unsigned)(r % LIVE);
        if (blk[i] != NULL) {
            unsigned char v = (unsigned char)(i * 7u + len[i]);
            ok &= blk[i][0] == v && blk[i][len[i] / 2] == v && blk[i][len[i] - 1] == v;
            free(blk[i]);
            blk[i] = NULL;
        } else {
            len[i] = 1 + (size_t)((r >> 20) % 4096u);
            blk[i] = malloc(len[i]);
            ok &= blk[i] != NULL;
            if (blk[i] != NULL) {
                memset(blk[i], (int)(unsigned char)(i * 7u + len[i]), len[i]);
                ok &= (char *)blk[i] >= lo && (char *)blk[i] + len[i] <= (char *)sbrk(0);
            }
        }
    }
    char *b1 = sbrk(0);
    for (unsigned i = 0; i < LIVE; i++) {
        free(blk[i]);
        blk[i] = NULL;
    }
    if (!ok || b1 - b0 <= 128 * 1024) {
        char line[160];
        (void)snprintf(line, sizeof line, "glibc: the break from %p to %p, its last op %u\n", (void *)b0,
                       (void *)b1, op_at);
        say(line);
    }
    return ok && b1 - b0 > 128 * 1024;
}

int main(int argc, char **argv)
{
    brk_at_main = sbrk(0);
    if (argc == 2 && strcmp(argv[1], "iobench") == 0) {
        return iobench();
    }
    say("glibc: main reached\n");
    check(argc == 2 && strcmp(argv[0], "glibctest") == 0 && strcmp(argv[1], "joist") == 0, "argv");
    const char *e = getenv("JOIST");
    check(e != NULL && strcmp(e, "phase5") == 0, "environment");

    /* First, on a heap not used yet: its growth is the test's. */
    int region_ok = malloc_in_region();
    /* malloc: small ones, then large ones (mmap'd: mm's MAP_ANON). */
    int heap_ok = 1;
    for (size_t n = 1; n <= 1u << 19; n <<= 1) {
        unsigned char *p = malloc(n);
        if (p == NULL) {
            heap_ok = 0;
            break;
        }
        memset(p, (int)(n & 0xff), n);
        heap_ok &= p[0] == (n & 0xff) && p[n - 1] == (n & 0xff);
        free(p);
    }
    int *z = calloc(1000, sizeof *z);
    heap_ok &= z != NULL;
    for (int i = 0; z != NULL && i < 1000; i++) {
        heap_ok &= z[i] == 0;
    }
    int *big = realloc(z, 100000 * sizeof *z);
    heap_ok &= big != NULL;
    free(big != NULL ? big : z);
    check(heap_ok, "malloc");
    check(region_ok, "malloc in a region");

    char buf[64];
    (void)snprintf(buf, sizeof buf, "%d %s %#x %.3f", 42, "joist", 255, 2.5);
    check(strcmp(buf, "42 joist 0xff 2.500") == 0, "snprintf");

    struct timespec t0, t1;
    int ck = clock_gettime(CLOCK_MONOTONIC, &t0) == 0;
    for (volatile int i = 0; i < 1000000; i++) {
    }
    ck &= clock_gettime(CLOCK_MONOTONIC, &t1) == 0;
    ck &= t1.tv_sec > t0.tv_sec || (t1.tv_sec == t0.tv_sec && t1.tv_nsec >= t0.tv_nsec);
    check(ck, "clock_gettime");
    /* A sleep waits its time (the Hurd's waits in mach_msg, which Joist
     * doesn't provide: glibc/sysdeps/joist/clock_nanosleep.c). */
    struct timespec nap = {0, 20000000}; /* 20 ms */
    ck = clock_gettime(CLOCK_MONOTONIC, &t0) == 0 && nanosleep(&nap, NULL) == 0 &&
         usleep(10000) == 0 && clock_gettime(CLOCK_MONOTONIC, &t1) == 0;
    ck &= (t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec) >= 30000000LL;
    check(ck, "nanosleep");
    check(sysconf(_SC_NPROCESSORS_ONLN) >= 1, "sysconf");
    check(tls_value == 7, "tls");
    /* An RPC with no server behind it (the task's own capability) fails. */
    struct task_basic_info tbi;
    mach_msg_type_number_t tn = TASK_BASIC_INFO_COUNT;
    kern_return_t tr = task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&tbi, &tn);
    {
        char line[80];
        (void)snprintf(line, sizeof line, "glibc: task_info gave %d\n", (int)tr);
        say(line);
    }
    check(tr != KERN_SUCCESS, "rpc with no server");
    /* fork is refused in Phase 5 (the design's item 10): an error, not a
     * hang (glibc/sysdeps/joist/_Fork.c). */
    check(fork() == -1 && errno == ENOSYS, "fork refused");

    /* stdout is the root's console: io_write through joistmig's stubs.
     * One write passes at most 4096 bytes (a short write: fd-write.c), and
     * stdio goes on from it. */
    check(printf("glibc: printf through the console\n") > 0 && fflush(stdout) == 0, "stdout");
    static char line6k[6000];
    memset(line6k, '=', sizeof line6k - 1);
    line6k[sizeof line6k - 1] = '\n';
    check(write(1, line6k, sizeof line6k) == 4096, "a short write");
    check(fwrite(line6k, 1, sizeof line6k, stdout) == sizeof line6k && fflush(stdout) == 0, "a long write");
    /* stdin is the console too: what its starter was given as its stdin,
     * JOIST_STDIN bytes (the selftest's: 200 KiB, each byte a hash of its
     * place), else nothing. Read by one read, far past the message:
     * io_read's data comes back through the write window (decision 0032),
     * a slice of libc's window region, which vm_deallocate and vm_protect
     * must then refuse. Then at its end. */
    const char *sl = getenv("JOIST_STDIN");
    long want = sl != NULL ? atol(sl) : 0;
    if (want > 0) {
        static unsigned char in_big[256 * 1024];
        ssize_t got = read(0, in_big, sizeof in_big);
        int ok = got == want;
        for (long i = 0; ok && i < want; i++) {
            ok = in_big[i] == stdin_byte((uint32_t)i);
        }
        if (!ok) {
            char line[80];
            (void)snprintf(line, sizeof line, "glibc: the long read gave %ld of %ld\n", (long)got, want);
            say(line);
        }
        check(ok, "a long read");
        check(window_region_held(), "window region held");
    }
    char in_c;
    check(read(0, &in_c, 1) == 0 && getchar() == EOF, "stdin at its end");
    say("\n");

    pthread_t th[THREADS];
    int created = 1;
    for (int i = 0; i < THREADS; i++) {
        created &= pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i) == 0;
    }
    check(created, "pthread_create");
    int joined = 1;
    for (int i = 0; created && i < THREADS; i++) {
        void *r;
        joined &= pthread_join(th[i], &r) == 0 && (intptr_t)r == (intptr_t)i * 3;
    }
    check(joined, "pthread_join");
    check(counter == (long)THREADS * ROUNDS, "mutex");
    check(arrived == THREADS && !tls_bad && tls_value == 7, "condition and thread tls");
    /* The pid its starter gave it (no proc server: the bootstrap port). */
    check(getpid() == 2, "pid");
    pthread_mutexattr_t ra;
    pthread_t rt;
    void *rr = NULL;
    int rok = pthread_mutexattr_init(&ra) == 0 &&
              pthread_mutexattr_setrobust(&ra, PTHREAD_MUTEX_ROBUST) == 0 &&
              pthread_mutex_init(&robust, &ra) == 0 && pthread_mutex_lock(&robust) == 0 &&
              pthread_create(&rt, NULL, try_robust, NULL) == 0 && pthread_join(rt, &rr) == 0;
    check(rok && (intptr_t)rr == EBUSY && pthread_mutex_unlock(&robust) == 0, "robust mutex excludes");
    check(!write_bad, "a thread's write");

    say(failed ? "glibc: FAILED\n" : "glibc: all ok\n");
    return failed;
}
