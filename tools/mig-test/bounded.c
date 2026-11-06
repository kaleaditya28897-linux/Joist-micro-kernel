/* SPDX-License-Identifier: GPL-3.0-only */
/* bounded.c - a bounded in-line out array through MIG's Joist mode
 * (thread_get_state, from gnumach's mach.defs): its state is the caller's
 * own buffer, *countInOut elements long, as MIG passes it. A fake
 * transport answers with N elements: fewer than the buffer holds, as many,
 * more (MIG's rule: what fits, the count set to all, MIG_ARRAY_TOO_LARGE),
 * and an impossible count (refused). io, fs and fsys have no such array,
 * so the round trip doesn't reach it. */
#include <joist_mig.h>
#include <mach/mach_types.h>
#include <mach/mig_errors.h>
#include <stdio.h>
#include <stdlib.h>

kern_return_t thread_get_state(thread_t target_thread, int flavor, thread_state_t old_state,
                               mach_msg_type_number_t *old_stateCnt);

static uint64_t answer_n;     /* elements the fake server sends */
static int answer_bad;        /* send a count no message could hold */
static unsigned failed;

static natural_t elem(uint64_t i)
{
    return (natural_t)(0x1000u + i * 7u);
}

int joist_mig_call(uint32_t port, struct joist_mig_msg *m)
{
    struct joist_mig_body b;
    if (port != 42 || (uint32_t)m->r[0] != 2017u || joist_mig_recv(m, &b) != 0) {
        return 1;
    }
    int flavor;
    if (joist_mig_get(&b, &flavor, sizeof flavor) != 0 || flavor != 5) {
        return 1;
    }
    b.at = b.end = 0;
    uint64_t n = answer_bad ? 1u << 20 : answer_n;
    joist_mig_put(&b, &n, 8);
    natural_t v[16]; /* the elements back to back, as an array goes */
    for (uint64_t i = 0; i < answer_n; i++) {
        v[i] = elem(i);
    }
    joist_mig_put(&b, v, answer_n * sizeof v[0]);
    m->r[0] = 0; /* KERN_SUCCESS */
    m->ncaps = 0;
    joist_mig_send(m, &b);
    return 0;
}

void *joist_mig_allocate(size_t n)
{
    return malloc(n);
}

void joist_mig_deallocate(void *p, size_t n)
{
    (void)n;
    free(p);
}

static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "bounded: %s\n", what);
        failed++;
    }
}

int main(void)
{
    natural_t buf[10];
    for (uint64_t n = 0; n <= 12; n++) {
        for (unsigned i = 0; i < 10; i++) {
            buf[i] = 0xdead;
        }
        answer_n = n;
        answer_bad = 0;
        mach_msg_type_number_t cnt = 8; /* the buffer is 8 long (2 more to see overruns) */
        kern_return_t r = thread_get_state(42, 5, buf, &cnt);
        check(cnt == n, "the count isn't what the server sent");
        check(r == (n > 8 ? MIG_ARRAY_TOO_LARGE : KERN_SUCCESS), "the wrong result");
        for (uint64_t i = 0; i < 10; i++) {
            check(buf[i] == (i < n && i < 8 ? elem(i) : 0xdead), "the buffer isn't what fits");
        }
    }
    answer_n = 3;
    answer_bad = 1;
    mach_msg_type_number_t cnt = 8;
    check(thread_get_state(42, 5, buf, &cnt) == MIG_TYPE_ERROR, "an impossible count accepted");
    printf("bounded: 13 replies and an impossible one; %u failed\n", failed);
    return failed != 0;
}
