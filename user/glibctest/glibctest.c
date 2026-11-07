/* SPDX-License-Identifier: GPL-3.0-only */
/* glibctest.c - the first glibc program on Joist (Phase 5 step 7, the
 * design's item 11 d): a static program linked with glibc's sysdeps/joist,
 * started by user/glibcroot as the Hurd's exec server would start it.
 *
 * It uses glibc as any program does and checks what it gets: argv and the
 * environment from start-up, malloc and friends (over mm), snprintf,
 * clock_gettime, sysconf's CPU count, thread-local variables, and pthreads
 * (htl over Joist threads): four threads contending on one mutex and
 * meeting at a condition variable. There is no console server yet, so it
 * reports through the kernel's debug write; each check prints
 * "glibc: <what> ok", and the last "glibc: all ok". Then it returns from
 * main, so exit and _exit end the task (task_terminate).
 *
 * Built by tools/phase5/glibc-test.sh with the cross-built glibc. */
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
static int arrived, tls_bad;

static void *worker(void *arg)
{
    int me = (int)(intptr_t)arg;
    tls_value = 100 + me; /* each thread its own */
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

int main(int argc, char **argv)
{
    say("glibc: main reached\n");
    check(argc == 2 && strcmp(argv[0], "glibctest") == 0 && strcmp(argv[1], "joist") == 0, "argv");
    const char *e = getenv("JOIST");
    check(e != NULL && strcmp(e, "phase5") == 0, "environment");

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

    say(failed ? "glibc: FAILED\n" : "glibc: all ok\n");
    return failed;
}
