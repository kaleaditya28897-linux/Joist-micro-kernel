/* SPDX-License-Identifier: GPL-3.0-only */
/* test_vm.c - host tests of glibc/sysdeps/joist/joist-vm.c (the task's
 * address reservations; Phase 5 step 7, the design's item 6), built small
 * (Makefile: 64 pages, a window of pages 8 to 48, 6 reservations).
 *
 * A reference model holds each page's state (free, or reserved with a
 * protection, and whether a reservation starts there). Directed tests
 * first; then seeded sequences of every call, ranges in and out of the
 * user half and the window, sizes up to past the end: each check
 * (__jv_can_*, __jv_find) must give the result the model predicts (a
 * free run's lowest page, a hole, no room for a cut), a call made only
 * when its check said yes (as glibc makes them), and after each step the
 * table must be well formed and say exactly what the model says, page by
 * page and reservation by reservation; vm_region is checked from every
 * page.
 *
 * usage: test-vm [SEEDS] */
#include "joist-vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if JV_USER_PAGES > 4096
#error "build test-vm small (see the Makefile)"
#endif
#define NP ((unsigned)JV_USER_PAGES)

static struct jv_table T;
static unsigned failed;

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "test-vm: %s:%d: failed: %s\n", __FILE__, __LINE__, #c); \
            failed++;                                                             \
        }                                                                         \
    } while (0)

/* The model: page p reserved (prot[p] >= 0) or free (-1); start[p] if a
 * reservation starts there. */
static int prot[NP + 1];
static unsigned char start[NP + 1];

static void model_init(void)
{
    for (unsigned p = 0; p <= NP; p++) {
        prot[p] = -1;
        start[p] = 0;
    }
    __jv_init(&T);
}

static unsigned entries(void)
{
    unsigned n = 0;
    for (unsigned p = 0; p < NP; p++) {
        n += start[p];
    }
    return n;
}

static int valid(uint64_t page, uint64_t pages)
{
    return page >= 1 && page < NP && pages <= NP - page;
}

/* What the model says after each change; also used to count the
 * entries a change would leave. */
static void m_reserve(uint64_t a, uint64_t n, int pr)
{
    for (uint64_t p = a; p < a + n; p++) {
        prot[p] = pr;
        start[p] = p == a;
    }
}
static void m_release(uint64_t a, uint64_t n)
{
    for (uint64_t p = a; p < a + n; p++) {
        prot[p] = -1;
        start[p] = 0;
    }
    if (a + n < NP && prot[a + n] >= 0) {
        start[a + n] = 1;
    }
}
static void m_protect(uint64_t a, uint64_t n, int pr)
{
    for (uint64_t p = a; p < a + n; p++) {
        prot[p] = pr;
    }
    start[a] = 1;
    if (a + n < NP && prot[a + n] >= 0) {
        start[a + n] = 1;
    }
}

/* The table agrees with the model, entry by entry. */
static int agree(void)
{
    if (__jv_check(&T) != 0) {
        return __LINE__;
    }
    unsigned i = 0;
    for (unsigned p = 0; p < NP; p++) {
        if (prot[p] < 0) {
            continue;
        }
        if (start[p]) {
            if (i >= T.n || T.r[i].page != p) {
                return __LINE__;
            }
            i++;
        }
        if (i == 0 || p >= T.r[i - 1].page + T.r[i - 1].pages || T.r[i - 1].prot != (uint32_t)prot[p]) {
            return __LINE__;
        }
        if (p + 1 < NP && prot[p + 1] < 0 && p + 1 != T.r[i - 1].page + T.r[i - 1].pages) {
            return __LINE__; /* the entry runs on past its pages */
        }
    }
    if (i != T.n) {
        return __LINE__;
    }
    /* vm_region from every page. */
    for (unsigned p = 0; p < NP; p++) {
        struct jv_res r;
        int e = __jv_region(&T, p, &r);
        unsigned q = p;
        while (q < NP && prot[q] < 0) {
            q++;
        }
        if (q == NP) {
            if (e != JV_NO_SPACE) {
                return __LINE__;
            }
            continue;
        }
        unsigned s = q;
        while (!start[s]) {
            s--;
        }
        if (e != JV_OK || r.page != s || r.prot != (uint32_t)prot[q]) {
            return __LINE__;
        }
    }
    return 0;
}

/* The model's predictions. */
static int predict_find(uint64_t n, uint64_t align, uint64_t *page)
{
    if (n == 0 || align == 0 || (align & (align - 1)) != 0) {
        return JV_INVALID_ARGUMENT;
    }
    for (uint64_t a = JV_LO; n <= JV_HI - JV_LO && a <= JV_HI - n; a++) {
        if (a % align != 0) {
            continue;
        }
        uint64_t p = a;
        while (p < a + n && prot[p] < 0) {
            p++;
        }
        if (p == a + n) {
            *page = a;
            return JV_OK;
        }
    }
    return JV_NO_SPACE;
}
static int predict_reserve(uint64_t a, uint64_t n)
{
    if (n == 0) {
        return JV_INVALID_ARGUMENT;
    }
    if (!valid(a, n)) {
        return JV_INVALID_ADDRESS;
    }
    for (uint64_t p = a; p < a + n; p++) {
        if (prot[p] >= 0) {
            return JV_NO_SPACE;
        }
    }
    return entries() == JV_MAX ? JV_RESOURCE_SHORTAGE : JV_OK;
}
/* Entries after a release or protect, on a copy of the model. */
static unsigned entries_after(int protect, uint64_t a, uint64_t n)
{
    int sp[NP + 1];
    unsigned char ss[NP + 1];
    memcpy(sp, prot, sizeof sp);
    memcpy(ss, start, sizeof ss);
    if (protect) {
        m_protect(a, n, 0);
    } else {
        m_release(a, n);
    }
    unsigned e = entries();
    memcpy(prot, sp, sizeof sp);
    memcpy(start, ss, sizeof ss);
    return e;
}
static int predict_release(uint64_t a, uint64_t n)
{
    if (n == 0) {
        return JV_OK;
    }
    if (!valid(a, n)) {
        return JV_INVALID_ADDRESS;
    }
    return entries_after(0, a, n) > JV_MAX ? JV_RESOURCE_SHORTAGE : JV_OK;
}
static int predict_protect(uint64_t a, uint64_t n)
{
    if (n == 0) {
        return JV_OK;
    }
    if (!valid(a, n)) {
        return JV_INVALID_ADDRESS;
    }
    for (uint64_t p = a; p < a + n; p++) {
        if (prot[p] < 0) {
            return JV_INVALID_ADDRESS;
        }
    }
    return entries_after(1, a, n) > JV_MAX ? JV_RESOURCE_SHORTAGE : JV_OK;
}

static void directed(void)
{
    uint64_t at;
    model_init();
    /* Anywhere: first fit from the window's start. */
    CHECK(__jv_find(&T, 4, 1, &at) == JV_OK && at == JV_LO);
    CHECK(__jv_find(&T, 4, 3, &at) == JV_INVALID_ARGUMENT);
    CHECK(__jv_can_reserve(&T, at, 4) == JV_OK);
    __jv_reserve(&T, at, 4, 3);
    m_reserve(at, 4, 3);
    CHECK(__jv_find(&T, 2, 1, &at) == JV_OK && at == JV_LO + 4);
    CHECK(__jv_find(&T, 2, 8, &at) == JV_OK && at == JV_LO + 8);
    /* Fixed, below the window, and an overlap refused. */
    CHECK(__jv_can_reserve(&T, 2, 3) == JV_OK);
    __jv_reserve(&T, 2, 3, 1);
    m_reserve(2, 3, 1);
    CHECK(__jv_can_reserve(&T, 4, 2) == JV_NO_SPACE);
    CHECK(__jv_can_reserve(&T, 0, 1) == JV_INVALID_ADDRESS);
    CHECK(__jv_can_reserve(&T, NP - 1, 2) == JV_INVALID_ADDRESS);
    CHECK(__jv_can_reserve(&T, 5, UINT64_MAX) == JV_INVALID_ADDRESS);
    CHECK(__jv_find(&T, JV_HI - JV_LO + 1, 1, &at) == JV_NO_SPACE);
    CHECK(agree() == 0);
    /* A release inside one reservation cuts it in two; the hole is found. */
    CHECK(__jv_can_release(&T, JV_LO + 1, 2) == JV_OK);
    __jv_release(&T, JV_LO + 1, 2);
    m_release(JV_LO + 1, 2);
    CHECK(T.n == 3 && agree() == 0);
    CHECK(__jv_find(&T, 2, 1, &at) == JV_OK && at == JV_LO + 1);
    /* Protect over a hole refused; across two reservations, cut at both
     * ends. */
    CHECK(__jv_can_protect(&T, JV_LO, 4) == JV_INVALID_ADDRESS);
    CHECK(__jv_can_protect(&T, 3, 2) == JV_OK);
    __jv_protect(&T, 3, 2, 7);
    m_protect(3, 2, 7);
    CHECK(T.n == 4 && agree() == 0);
    /* A release across reservations and holes. */
    CHECK(__jv_can_release(&T, 1, JV_LO + 2) == JV_OK);
    __jv_release(&T, 1, JV_LO + 2);
    m_release(1, JV_LO + 2);
    CHECK(T.n == 1 && agree() == 0);
    /* Full: no reservation, no cut, but a release that cuts nothing. */
    model_init();
    for (unsigned i = 0; i < JV_MAX; i++) {
        CHECK(__jv_can_reserve(&T, 1 + 4u * i, 3) == JV_OK);
        __jv_reserve(&T, 1 + 4u * i, 3, 0);
        m_reserve(1 + 4u * i, 3, 0);
    }
    CHECK(__jv_can_reserve(&T, 40, 1) == JV_RESOURCE_SHORTAGE);
    CHECK(__jv_can_release(&T, 2, 1) == JV_RESOURCE_SHORTAGE);
    CHECK(__jv_can_protect(&T, 2, 1) == JV_RESOURCE_SHORTAGE);
    CHECK(__jv_can_release(&T, 1, 2) == JV_OK);
    CHECK(agree() == 0);
}

static uint64_t rng;
static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}
static uint64_t rpage(void)
{
    switch (rnd() % 8u) {
    case 0:
        return 0;
    case 1:
        return NP - 1u - rnd() % 3u;
    case 2:
        return NP + rnd() % 4u;
    default:
        return 1 + rnd() % (NP - 1u);
    }
}
static uint64_t rpages(void)
{
    switch (rnd() % 10u) {
    case 0:
        return 0;
    case 1:
        return UINT64_MAX - rnd() % 4u;
    case 2:
        return NP;
    default:
        return 1 + rnd() % 8u;
    }
}

static unsigned counts[5][8];

static void property(unsigned seeds)
{
    for (unsigned seed = 1; seed <= seeds; seed++) {
        model_init();
        rng = seed * 0x9e3779b97f4a7c15ull;
        for (unsigned step = 0; step < 300; step++) {
            uint64_t a = rpage(), n = rpages(), at = 0, want = 0;
            int pr = (int)(rnd() % 8u), op = (int)(rnd() % 5u), e, w;
            switch (op) {
            case 0: /* anywhere */
            case 1:
                n = n > 16 ? 1 + n % 16 : n;
                uint64_t align = rnd() % 8u == 0 ? 3 : 1ull << (rnd() % 5u);
                e = __jv_find(&T, n, align, &at);
                w = predict_find(n, align, &want);
                CHECK(e == w && (e != JV_OK || at == want));
                if (e == JV_OK) {
                    e = __jv_can_reserve(&T, at, n);
                    CHECK(e == predict_reserve(at, n));
                    if (e == JV_OK) {
                        __jv_reserve(&T, at, n, (uint32_t)pr);
                        m_reserve(at, n, pr);
                    }
                }
                break;
            case 2: /* fixed */
                e = __jv_can_reserve(&T, a, n);
                CHECK(e == predict_reserve(a, n));
                if (e == JV_OK) {
                    __jv_reserve(&T, a, n, (uint32_t)pr);
                    m_reserve(a, n, pr);
                }
                break;
            case 3:
                e = __jv_can_release(&T, a, n);
                CHECK(e == predict_release(a, n));
                if (e == JV_OK) {
                    __jv_release(&T, a, n);
                    if (n != 0) {
                        m_release(a, n);
                    }
                }
                break;
            default:
                e = __jv_can_protect(&T, a, n);
                CHECK(e == predict_protect(a, n));
                if (e == JV_OK) {
                    __jv_protect(&T, a, n, (uint32_t)pr);
                    if (n != 0) {
                        m_protect(a, n, pr);
                    }
                }
                break;
            }
            counts[op][e & 7]++;
            int line = agree();
            if (line != 0) {
                fprintf(stderr, "test-vm: seed %u step %u: op %d (%d): disagrees (%d)\n", seed, step, op, e,
                        line);
                failed++;
                return;
            }
        }
    }
}

int main(int argc, char **argv)
{
    unsigned seeds = argc > 1 ? (unsigned)atoi(argv[1]) : 2000u;
    directed();
    property(seeds);
    static const char *const names[5] = {"anywhere", "anywhere", "fixed", "release", "protect"};
    for (unsigned op = 1; op < 5; op++) {
        unsigned *c = counts[op];
        if (op == 1) {
            for (unsigned k = 0; k < 8; k++) {
                c[k] += counts[0][k];
            }
        }
        fprintf(stderr, "test-vm: %-8s OK=%u INVALID_ADDRESS=%u NO_SPACE=%u INVALID_ARGUMENT=%u "
                        "RESOURCE_SHORTAGE=%u\n",
                names[op], c[0], c[1], c[3], c[4], c[6]);
        /* Every outcome each call has, reached. */
        CHECK(c[0] > 0 && c[6] > 0 && (op == 1 || op == 3 || c[1] > 0));
    }
    CHECK(counts[2][3] > 0 && counts[1][3] > 0);
    if (failed != 0) {
        fprintf(stderr, "test-vm: %u failed\n", failed);
        return 1;
    }
    fprintf(stderr, "test-vm: ok (%u seeds)\n", seeds);
    return 0;
}
