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
 * The vm_* calls (__jv_allocate, __jv_deallocate, __jv_vm_protect,
 * __jv_vm_region: bytes and Mach's protections) run on a fake mm that
 * records each request and refuses some (each of mm's results): a call
 * must ask mm just when the table's check said yes, with the pages and
 * permissions predicted, give mm's refusal as its Mach result, and leave
 * the table as it was on any refusal.
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

/* The fake mm: the last request, how many, and what to answer. */
static struct {
    int req;
    uint64_t page, pages;
    uint32_t perms;
    unsigned calls;
    int answer;
} mm;

int __joist_vm_mm(int req, uint64_t page, uint64_t pages, uint32_t perms)
{
    mm.req = req;
    mm.page = page;
    mm.pages = pages;
    mm.perms = perms;
    mm.calls++;
    return mm.answer;
}

#define PG(p) ((uint64_t)(p) << JV_PAGE_SHIFT)

static void directed_calls(void)
{
    uint64_t at, size;
    uint32_t pr;
    model_init();
    mm.answer = 0;
    /* Anywhere, rounded up to pages; read-write is mm's W. */
    at = 12345;
    mm.calls = 0;
    CHECK(__jv_allocate(&T, &at, 1, 0, 1, JV_PROT_READ | JV_PROT_WRITE) == JV_OK);
    CHECK(at == PG(JV_LO) && mm.calls == 1 && mm.req == JV_MM_MAP_ANON && mm.page == JV_LO &&
          mm.pages == 1 && mm.perms == JV_MM_W);
    m_reserve(JV_LO, 1, 3);
    /* Fixed, an address inside a page and a size past its end: two pages. */
    at = PG(2) + 100;
    CHECK(__jv_allocate(&T, &at, PG(1), 0, 0, JV_PROT_READ | JV_PROT_EXECUTE) == JV_OK);
    CHECK(at == PG(2) && mm.page == 2 && mm.pages == 2 && mm.perms == JV_MM_X);
    m_reserve(2, 2, 5);
    /* vm_map's mask: 4 pages' alignment. */
    at = 0;
    CHECK(__jv_allocate(&T, &at, PG(1), PG(4) - 1, 1, JV_PROT_READ) == JV_OK);
    CHECK(at == PG(JV_LO + 4) && mm.perms == 0);
    m_reserve(JV_LO + 4, 1, 1);
    CHECK(__jv_allocate(&T, &at, PG(1), PG(3) - 1, 1, JV_PROT_READ) == JV_INVALID_ARGUMENT);
    /* No access is mm's NONE; write and execute together refused, mm not
     * asked. */
    at = PG(6);
    CHECK(__jv_allocate(&T, &at, 1, 0, 0, 0) == JV_OK && mm.perms == JV_MM_NONE);
    m_reserve(6, 1, 0);
    mm.calls = 0;
    at = PG(7);
    CHECK(__jv_allocate(&T, &at, 1, 0, 0, 7) == JV_PROTECTION_FAILURE);
    CHECK(__jv_allocate(&T, &at, 0, 0, 0, 3) == JV_INVALID_ARGUMENT);
    CHECK(__jv_allocate(&T, &at, UINT64_MAX, 0, 0, 3) == JV_INVALID_ARGUMENT);
    at = PG(3);
    CHECK(__jv_allocate(&T, &at, 1, 0, 0, 3) == JV_NO_SPACE && at == PG(3));
    CHECK(__jv_vm_protect(&T, PG(2), 1, 6) == JV_PROTECTION_FAILURE);
    CHECK(__jv_vm_protect(&T, PG(4), 1, 1) == JV_INVALID_ADDRESS); /* a hole */
    CHECK(__jv_deallocate(&T, PG(NP), 1) == JV_INVALID_ADDRESS);
    CHECK(__jv_deallocate(&T, 5, UINT64_MAX) == JV_INVALID_ADDRESS);
    CHECK(mm.calls == 0 && agree() == 0);
    /* mm refuses: its result as Mach's, the table unchanged. */
    static const int mach[5] = {JV_OK, JV_INVALID_ADDRESS, JV_RESOURCE_SHORTAGE, JV_NO_SPACE, JV_FAILURE};
    for (int a = 1; a <= 5; a++) {
        mm.answer = a;
        at = PG(9);
        CHECK(__jv_allocate(&T, &at, 1, 0, 0, 3) == mach[a < 5 ? a : 4] && at == PG(9));
        CHECK(__jv_deallocate(&T, PG(2), PG(2)) == mach[a < 5 ? a : 4] && mm.req == JV_MM_UNMAP);
        CHECK(__jv_vm_protect(&T, PG(2), 1, 1) == mach[a < 5 ? a : 4] && mm.req == JV_MM_PROTECT);
        CHECK(agree() == 0);
    }
    mm.answer = 0;
    /* Deallocate of a range partly reserved: mm unmaps it all; size 0 does
     * nothing. */
    mm.calls = 0;
    CHECK(__jv_deallocate(&T, PG(3), 0) == JV_OK && mm.calls == 0);
    CHECK(__jv_deallocate(&T, PG(3) + 1, PG(4)) == JV_OK && mm.page == 3 && mm.pages == 5);
    m_release(3, 5);
    CHECK(agree() == 0);
    /* Protect, then vm_region from an address inside and below. */
    CHECK(__jv_vm_protect(&T, PG(2) + 7, 1, JV_PROT_READ) == JV_OK && mm.page == 2 && mm.pages == 1 &&
          mm.perms == 0);
    m_protect(2, 1, 1);
    at = PG(2) + 9;
    CHECK(__jv_vm_region(&T, &at, &size, &pr) == JV_OK && at == PG(2) && size == PG(1) && pr == 1);
    at = PG(3);
    CHECK(__jv_vm_region(&T, &at, &size, &pr) == JV_OK && at == PG(JV_LO) && size == PG(1) && pr == 3);
    at = PG(JV_LO + 5);
    CHECK(__jv_vm_region(&T, &at, &size, &pr) == JV_NO_SPACE);
    CHECK(agree() == 0);
}

/* mm's permissions for a protection, or -1 if refused. */
static int predict_perms(uint32_t pr)
{
    if ((pr & 6u) == 6u) {
        return -1;
    }
    if ((pr & 7u) == 0) {
        return (int)JV_MM_NONE;
    }
    return (int)((pr & 2u ? JV_MM_W : 0) | (pr & 4u ? JV_MM_X : 0));
}

static unsigned calls_counts[4][8];

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

/* Mach's result for mm's. */
static int mach_of(int a)
{
    static const int mach[6] = {JV_OK,       JV_INVALID_ADDRESS, JV_RESOURCE_SHORTAGE,
                                JV_NO_SPACE, JV_FAILURE,         JV_FAILURE /* not one of mm's */};
    return mach[a];
}

static uint64_t raddr(void)
{
    return PG(rpage()) + (rnd() % 4u == 0 ? rnd() % PG(1) : 0);
}
static uint64_t rsize(void)
{
    uint64_t n = rpages();
    if (n > NP) {
        return n; /* past the end in bytes too */
    }
    return rnd() % 4u == 0 && n > 0 ? PG(n) - rnd() % PG(1) : PG(n);
}

/* Seeded sequences of the vm_* calls, mm refusing one in five. */
static void property_calls(unsigned seeds)
{
    static struct jv_table before;
    for (unsigned seed = 1; seed <= seeds; seed++) {
        model_init();
        rng = seed * 0xd1b54a32d192ed03ull;
        for (unsigned step = 0; step < 300; step++) {
            uint64_t addr = raddr(), size = rsize(), mask = 0, at = addr;
            uint32_t vp = (uint32_t)(rnd() % 8u);
            int op = (int)(rnd() % 4u), e, w = JV_OK;
            int perms = predict_perms(vp);
            /* The pages of [addr, addr + size), or none if past the end. */
            uint64_t off = addr & (PG(1) - 1), a = addr >> JV_PAGE_SHIFT, n = 0;
            int fits = size <= UINT64_MAX - off - (PG(1) - 1);
            if (fits) {
                n = (off + size + PG(1) - 1) >> JV_PAGE_SHIFT;
            }
            mm.answer = rnd() % 5u == 0 ? 1 + (int)(rnd() % 5u) : 0;
            mm.calls = 0;
            before = T;
            int mm_req = 0;
            switch (op) {
            case 0: /* anywhere, and vm_map's masks */
                size = size > PG(16) ? 1 + size % PG(16) : size;
                n = (size + PG(1) - 1) >> JV_PAGE_SHIFT;
                switch (rnd() % 4u) {
                case 0:
                    mask = rnd() % PG(1);
                    mask = (mask & (mask + 1)) == 0 ? mask : PG(1) - 1;
                    break;
                case 1:
                    mask = PG(1ull << (rnd() % 5u)) - 1;
                    break;
                case 2:
                    mask = PG(3) - 1;
                    break;
                default:
                    mask = rnd() % 8u == 0 ? UINT64_MAX : 0;
                }
                e = __jv_allocate(&T, &at, size, mask, 1, vp);
                if (size == 0) {
                    w = JV_INVALID_ARGUMENT;
                } else if (perms < 0) {
                    w = JV_PROTECTION_FAILURE;
                } else if ((mask & (mask + 1)) != 0) {
                    w = JV_INVALID_ARGUMENT;
                } else {
                    uint64_t align = (mask >> JV_PAGE_SHIFT) + 1;
                    w = predict_find(n, align, &a);
                    if (w == JV_OK) {
                        w = predict_reserve(a, n);
                    }
                    mm_req = w == JV_OK ? JV_MM_MAP_ANON : 0;
                }
                break;
            case 1: /* fixed */
                e = __jv_allocate(&T, &at, size, 0, 0, vp);
                if (size == 0 || !fits) {
                    w = JV_INVALID_ARGUMENT;
                } else if (perms < 0) {
                    w = JV_PROTECTION_FAILURE;
                } else {
                    w = predict_reserve(a, n);
                    mm_req = w == JV_OK ? JV_MM_MAP_ANON : 0;
                }
                break;
            case 2:
                e = __jv_deallocate(&T, addr, size);
                if (size != 0) {
                    w = !fits ? JV_INVALID_ADDRESS : predict_release(a, n);
                    mm_req = w == JV_OK ? JV_MM_UNMAP : 0;
                }
                break;
            default:
                e = __jv_vm_protect(&T, addr, size, vp);
                if (size != 0) {
                    w = !fits ? JV_INVALID_ADDRESS : perms < 0 ? JV_PROTECTION_FAILURE : predict_protect(a, n);
                    mm_req = w == JV_OK ? JV_MM_PROTECT : 0;
                }
                break;
            }
            if (mm_req != 0) {
                CHECK(mm.calls == 1 && mm.req == mm_req && mm.page == a && mm.pages == n &&
                      (mm_req == JV_MM_UNMAP || mm.perms == (uint32_t)perms));
                w = mach_of(mm.answer);
            } else {
                CHECK(mm.calls == 0);
            }
            CHECK(e == w);
            if (e != JV_OK) {
                CHECK(memcmp(&before, &T, sizeof T) == 0 && at == addr);
            } else if (op <= 1) {
                CHECK(at == PG(a));
                m_reserve(a, n, (int)vp);
            } else if (op == 2 && size != 0) {
                m_release(a, n);
            } else if (op == 3 && size != 0) {
                m_protect(a, n, (int)vp);
            }
            calls_counts[op][e & 7]++;
            /* vm_region from the address. */
            uint64_t ra = addr, rs = 0;
            uint32_t rp = 0;
            struct jv_res r;
            int re = __jv_region(&T, addr >> JV_PAGE_SHIFT, &r);
            CHECK(__jv_vm_region(&T, &ra, &rs, &rp) == re &&
                  (re != JV_OK || (ra == PG(r.page) && rs == PG(r.pages) && rp == r.prot)));
            int line = agree();
            if (line != 0) {
                fprintf(stderr, "test-vm: calls, seed %u step %u: op %d (%d): disagrees (%d)\n", seed, step, op,
                        e, line);
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
    directed_calls();
    property_calls(seeds);
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
    static const char *const cnames[4] = {"allocate", "fixed", "dealloc", "vm_prot"};
    for (unsigned op = 0; op < 4; op++) {
        unsigned *c = calls_counts[op];
        fprintf(stderr, "test-vm: %-8s OK=%u INVALID_ADDRESS=%u PROTECTION_FAILURE=%u NO_SPACE=%u "
                        "INVALID_ARGUMENT=%u FAILURE=%u RESOURCE_SHORTAGE=%u\n",
                cnames[op], c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
        /* Every result each call has, reached. */
        CHECK(c[0] > 0 && c[1] > 0 && c[3] > 0 && c[5] > 0 && c[6] > 0);
        CHECK(op == 2 || c[2] > 0);
        CHECK(op >= 2 || c[4] > 0);
    }
    if (failed != 0) {
        fprintf(stderr, "test-vm: %u failed\n", failed);
        return 1;
    }
    fprintf(stderr, "test-vm: ok (%u seeds)\n", seeds);
    return 0;
}
