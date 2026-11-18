/* SPDX-License-Identifier: GPL-3.0-only */
/* test_winmem.c - glibc/sysdeps/joist/joist-winmem.c (the pages of libc's
 * write-window region; Phase 5 step 9, the owner's choice of 2026-11-16).
 *
 * A reference holds each page of the region: not grown yet, free, or in
 * slice k (each slice's pages and first page kept too). After every
 * operation the table must be well formed (__jw_check) and say exactly
 * what the reference says: its runs are the reference's maximal free runs,
 * its mapped part and extents and slices out the same. Every result is
 * predicted from the reference first: __jw_take the lowest free run that
 * holds the slice (first fit), __jw_plan the extents to grow (none if a run
 * holds it, else as many as make the free tail hold it), __jw_give a merge
 * of a slice out and nothing else. A refused operation must change nothing.
 * Never do two slices share a page: a page taken must be free in the
 * reference.
 *
 * Built at glibc's sizes (JW_FIRST 256 pages, 7 extents, 32 runs) and, in
 * the `small' variant (model/Makefile), at tiny ones, where the region and
 * the table fill all the time. */
#include "harness.h"
#include "joist-winmem.h"

#include <stdlib.h>
#include <string.h>

#define NP JW_PAGES
enum { R_UNGROWN = -2, R_FREE = -1 }; /* else the slice's index */

static struct jw_table T;
static int pg[NP];
static struct {
    uint32_t page, pages;
} sl[JW_RUNS];
static uint32_t nsl, r_mapped, r_extents;

static uint32_t ext_size(uint32_t k)
{
    return k == 0 ? JW_FIRST : JW_FIRST << (k - 1u);
}

static void ref_init(void)
{
    __jw_init(&T);
    for (uint32_t p = 0; p < NP; p++) {
        pg[p] = R_UNGROWN;
    }
    nsl = r_mapped = r_extents = 0;
}

static void ref_grow(void)
{
    uint32_t n = ext_size(r_extents++);
    for (uint32_t p = r_mapped; p < r_mapped + n; p++) {
        pg[p] = R_FREE;
    }
    r_mapped += n;
}

/* The reference's maximal free run from p: its length (0 if p isn't the
 * first page of one). */
static uint32_t run_at(uint32_t p)
{
    if (pg[p] != R_FREE || (p > 0 && pg[p - 1u] == R_FREE)) {
        return 0;
    }
    uint32_t e = p;
    while (e < NP && pg[e] == R_FREE) {
        e++;
    }
    return e - p;
}

/* First fit, by the reference: the first page of the lowest free run that
 * holds n pages, or -1. */
static int64_t ref_fit(uint32_t n)
{
    for (uint32_t p = 0; p < r_mapped; p++) {
        if (run_at(p) >= n) {
            return p;
        }
    }
    return -1;
}

/* The extents a slice of n pages needs grown, by the reference: -1 none
 * would do. */
static int ref_plan(uint32_t n)
{
    if (ref_fit(n) >= 0) {
        return 0;
    }
    uint32_t tail = 0;
    while (tail < r_mapped && pg[r_mapped - 1u - tail] == R_FREE) {
        tail++;
    }
    for (uint32_t k = r_extents; k < JW_EXTENTS; k++) {
        tail += ext_size(k);
        if (tail >= n) {
            return (int)(k + 1u - r_extents);
        }
    }
    return -1;
}

static void ref_take(uint32_t page, uint32_t n)
{
    for (uint32_t p = page; p < page + n; p++) {
        pg[p] = (int)nsl;
    }
    sl[nsl].page = page;
    sl[nsl].pages = n;
    nsl++;
}

static void ref_give(uint32_t k)
{
    for (uint32_t p = sl[k].page; p < sl[k].page + sl[k].pages; p++) {
        pg[p] = R_FREE;
    }
    nsl--;
    sl[k] = sl[nsl];
    for (uint32_t p = sl[k].page; k < nsl && p < sl[k].page + sl[k].pages; p++) {
        pg[p] = (int)k; /* the last slice's index moved to k */
    }
}

/* The table and the reference agree: 0, or the line that says how not. */
static int agree(void)
{
    if (__jw_check(&T) != 0) {
        fprintf(stderr, "  __jw_check: %d\n", __jw_check(&T));
        return __LINE__;
    }
    if (T.mapped != r_mapped || T.extents != r_extents || T.slices != nsl) {
        return __LINE__;
    }
    uint32_t i = 0;
    for (uint32_t p = 0; p < NP; p++) {
        if ((p < r_mapped) != (pg[p] != R_UNGROWN)) {
            return __LINE__;
        }
        uint32_t n = run_at(p);
        if (n == 0) {
            continue;
        }
        if (i == T.n || T.r[i].page != p || T.r[i].pages != n) {
            return __LINE__;
        }
        i++;
    }
    if (i != T.n) {
        return __LINE__;
    }
    for (uint32_t k = 0; k < nsl; k++) {
        for (uint32_t p = sl[k].page; p < sl[k].page + sl[k].pages; p++) {
            if (pg[p] != (int)k) {
                return __LINE__; /* two slices on a page */
            }
        }
    }
    return 0;
}

/* A slice of `n` as the runtime takes one: planned, the extents grown,
 * then taken (which must then succeed). Its result. */
static int take_as_runtime(uint32_t n, uint32_t *page)
{
    uint32_t grow = 99;
    int e = __jw_plan(&T, n, &grow);
    if (e != JW_OK) {
        return e;
    }
    for (uint32_t g = 0; g < grow; g++) {
        uint32_t x = __jw_extent_pages(&T);
        if (x == 0 || __jw_grow(&T, x) != JW_OK) {
            return -1;
        }
        ref_grow();
    }
    e = __jw_take(&T, n, page);
    return e == JW_OK ? JW_OK : -1;
}

JT_TEST(winmem_grows_by_doubling)
{
    ref_init();
    JT_CHECK(JW_PAGES == JW_FIRST * 64u || JW_EXTENTS != 7u);
    uint32_t total = 0;
    for (uint32_t k = 0; k < JW_EXTENTS; k++) {
        uint32_t x = __jw_extent_pages(&T);
        JT_CHECK(x == ext_size(k));
        JT_CHECK(__jw_grow(&T, x + 1u) == JW_INVALID); /* not this extent's size */
        JT_CHECK(__jw_grow(&T, 0) == JW_INVALID);
        JT_REQUIRE(__jw_grow(&T, x) == JW_OK);
        ref_grow();
        total += x;
        JT_CHECK(T.mapped == total && T.n == 1 && T.r[0].page == 0 && T.r[0].pages == total);
        JT_CHECK(agree() == 0);
    }
    JT_CHECK(total == JW_PAGES);
    JT_CHECK(__jw_extent_pages(&T) == 0 && __jw_grow(&T, JW_FIRST) == JW_INVALID);
#if JW_FIRST == 256u && JW_EXTENTS == 7u
    /* The owner's schedule: 1, 1, 2, 4, 8, 16, 32 MiB; 64 MiB, 32 tables. */
    JT_CHECK(JW_PAGES == 32u * 512u);
#endif
}

JT_TEST(winmem_plan_grows_only_as_needed)
{
    ref_init();
    uint32_t grow = 0, at = 0;
    JT_CHECK(__jw_plan(&T, 0, &grow) == JW_INVALID);
    JT_CHECK(__jw_plan(&T, 1, &grow) == JW_OK && grow == 1);
    JT_CHECK(__jw_plan(&T, JW_FIRST + 1u, &grow) == JW_OK && grow == 2);
    JT_CHECK(__jw_plan(&T, JW_PAGES, &grow) == JW_OK && grow == JW_EXTENTS);
    JT_CHECK(__jw_plan(&T, JW_PAGES + 1u, &grow) == JW_NONE);
    JT_CHECK(__jw_take(&T, 1, &at) == JW_NONE); /* nothing grown */
    JT_CHECK(agree() == 0);
    /* One slice of the first extent less a page: the next needs no more. */
    JT_REQUIRE(take_as_runtime(JW_FIRST - 1u, &at) == JW_OK && at == 0);
    ref_take(at, JW_FIRST - 1u);
    JT_CHECK(T.extents == 1 && agree() == 0);
    JT_CHECK(__jw_plan(&T, 1, &grow) == JW_OK && grow == 0);
    /* Two pages: the free tail (one page) and the next extent. */
    JT_CHECK(__jw_plan(&T, 2, &grow) == JW_OK && grow == 1);
    JT_REQUIRE(take_as_runtime(2, &at) == JW_OK && at == JW_FIRST - 1u);
    ref_take(at, 2);
    JT_CHECK(T.extents == 2 && agree() == 0);
}

JT_TEST(winmem_first_fit_and_merges)
{
    ref_init();
    uint32_t at = 0;
    JT_REQUIRE(__jw_grow(&T, JW_FIRST) == JW_OK);
    ref_grow();
    /* Four slices of a quarter each (two pages at least in `small'). */
    uint32_t q = JW_FIRST / 4u;
    for (uint32_t k = 0; k < 4u && k + 1u < JW_RUNS; k++) {
        JT_REQUIRE(__jw_take(&T, q, &at) == JW_OK && at == k * q);
        ref_take(at, q);
    }
    if (JW_RUNS < 5u) {
        return;
    }
    JT_CHECK(T.n == 0 && agree() == 0);
    /* All of it out: a range from the last slice on past the mapped part is
     * none of them. */
    JT_CHECK(__jw_give(&T, 3u * q, q + 1u) == JW_INVALID && agree() == 0);
    /* Free the second and the fourth: two runs; a slice of a quarter takes
     * the lower (first fit). */
    JT_CHECK(__jw_give(&T, q, q) == JW_OK);
    ref_give(1);
    JT_CHECK(__jw_give(&T, 3u * q, q) == JW_OK);
    ref_give(1); /* the fourth was moved to index 1 */
    JT_CHECK(T.n == 2 && agree() == 0);
    /* Given back twice, or partly free: refused, nothing changed. */
    struct jw_table before = T;
    JT_CHECK(__jw_give(&T, q, q) == JW_INVALID);
    JT_CHECK(__jw_give(&T, q - 1u, 2) == JW_INVALID);
    JT_CHECK(__jw_give(&T, 0, JW_FIRST) == JW_INVALID);
    JT_CHECK(__jw_give(&T, JW_FIRST, 1) == JW_INVALID); /* past the mapped part */
    JT_CHECK(__jw_give(&T, 0, 0) == JW_INVALID);
    JT_CHECK(memcmp(&before, &T, sizeof T) == 0);
    JT_REQUIRE(__jw_take(&T, q, &at) == JW_OK && at == q);
    ref_take(at, q);
    JT_CHECK(agree() == 0);
    /* The third back: it merges with the run after it; then the second
     * joins the run before... */
    JT_CHECK(__jw_give(&T, 2u * q, q) == JW_OK);
    for (uint32_t k = 0; k < nsl; k++) {
        if (sl[k].page == 2u * q) {
            ref_give(k);
            break;
        }
    }
    JT_CHECK(T.n == 1 && T.r[0].page == 2u * q && T.r[0].pages == 2u * q && agree() == 0);
    /* ...and the first and second together make one run of all. */
    JT_CHECK(__jw_give(&T, q, q) == JW_OK);
    for (uint32_t k = 0; k < nsl; k++) {
        if (sl[k].page == q) {
            ref_give(k);
            break;
        }
    }
    JT_CHECK(__jw_give(&T, 0, q) == JW_OK);
    ref_give(0);
    JT_CHECK(T.n == 1 && T.r[0].page == 0 && T.r[0].pages == JW_FIRST && T.slices == 0 && agree() == 0);
    JT_CHECK(__jw_give(&T, 0, 1) == JW_INVALID); /* no slice out */
}

JT_TEST(winmem_full_table_refuses)
{
    ref_init();
    uint32_t at = 0, grow = 0;
    /* Slices of a page each, as many as there may be: the next refused
     * before anything is grown or taken. */
    for (uint32_t k = 0; k + 1u < JW_RUNS; k++) {
        JT_REQUIRE(take_as_runtime(1, &at) == JW_OK);
        ref_take(at, 1);
    }
    struct jw_table before = T;
    JT_CHECK(__jw_plan(&T, 1, &grow) == JW_FULL);
    JT_CHECK(__jw_take(&T, 1, &at) == JW_FULL);
    JT_CHECK(memcmp(&before, &T, sizeof T) == 0 && agree() == 0);
    /* Every other one back: each a run of its own, the table as full as it
     * gets with slices out between them. */
    for (uint32_t p = 0; p + 1u < JW_RUNS; p += 2u) {
        JT_REQUIRE(__jw_give(&T, p, 1) == JW_OK);
        for (uint32_t k = 0; k < nsl; k++) {
            if (sl[k].page == p) {
                ref_give(k);
                break;
            }
        }
    }
    JT_CHECK(agree() == 0);
    /* A table made full by hand (JW_RUNS runs, a slice between each two:
     * a state the slices' bound keeps the calls from reaching): a slice
     * given back that would need a run of its own is refused, nothing
     * changed, and so is an extent that would. */
    struct jw_table f;
    __jw_init(&f);
    f.extents = JW_EXTENTS;
    f.mapped = JW_PAGES;
    f.n = JW_RUNS;
    f.slices = JW_RUNS - 1u;
    for (uint32_t i = 0; i < JW_RUNS; i++) {
        f.r[i].page = 3u * i;
        f.r[i].pages = 1;
    }
    struct jw_table g = f;
    JT_CHECK(__jw_give(&f, 3u * JW_RUNS, 1) == JW_FULL);
    JT_CHECK(memcmp(&f, &g, sizeof f) == 0);
    /* Next to a run, it merges: room or not. */
    JT_CHECK(__jw_give(&f, 1, 1) == JW_OK && f.n == JW_RUNS && f.r[0].pages == 2);
    f = g;
    f.extents = JW_EXTENTS - 1u;
    f.mapped = JW_PAGES - ext_size(JW_EXTENTS - 1u);
    g = f;
    JT_CHECK(__jw_grow(&f, ext_size(JW_EXTENTS - 1u)) == JW_FULL && memcmp(&f, &g, sizeof f) == 0);
}

/* __jw_check finds each rule broken (the property test relies on it). */
JT_TEST(winmem_check_finds_breaks)
{
    struct jw_table g, b;
    __jw_init(&g);
    JT_REQUIRE(__jw_grow(&g, JW_FIRST) == JW_OK && __jw_grow(&g, JW_FIRST) == JW_OK);
    uint32_t at = 0;
    JT_REQUIRE(__jw_take(&g, 1, &at) == JW_OK && __jw_take(&g, 1, &at) == JW_OK);
    JT_REQUIRE(__jw_give(&g, 0, 1) == JW_OK); /* runs [0, 1) and [2, mapped) */
    JT_REQUIRE(__jw_check(&g) == 0 && g.n == 2);
    b = g;
    b.mapped++;
    JT_CHECK(__jw_check(&b) == 4);
    b = g;
    b.r[0].pages = 0;
    JT_CHECK(__jw_check(&b) == 5);
    b = g;
    b.r[1].pages++;
    JT_CHECK(__jw_check(&b) == 6); /* past the mapped part */
    b = g;
    b.r[0].pages = 2; /* touching the next */
    JT_CHECK(__jw_check(&b) == 7);
    b = g;
    b.r[0].pages = 3; /* overlapping it */
    JT_CHECK(__jw_check(&b) == 7);
    b = g;
    b.slices = 0;
    JT_CHECK(__jw_check(&b) == 8);
    b = g;
    b.slices = 2;
    b.r[1].page = 3;
    b.r[1].pages--;
    JT_CHECK(__jw_check(&b) == 0); /* two slices, two pages out */
    b.slices = 3;
    JT_CHECK(__jw_check(&b) == 10);
    __jw_init(&b);
    JT_REQUIRE(__jw_grow(&b, JW_FIRST) == JW_OK);
    b.r[0].pages--;
    JT_CHECK(__jw_check(&b) == 9); /* none out, a page not free */
}

/* The property test: seeded sequences of every operation. */
#define SEEDS 60u
#define STEPS 400u

static uint64_t rng;
static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* A slice's pages: small mostly, now and then up to the region and past. */
static uint32_t rsize(void)
{
    switch (rnd() % 10u) {
    case 0:
        return 0;
    case 1:
        return (uint32_t)(1u + rnd() % (NP + 2u));
    case 2:
        return (uint32_t)(1u + rnd() % (JW_FIRST * 4u));
    default:
        return (uint32_t)(1u + rnd() % (JW_FIRST / 2u + 1u));
    }
}

static unsigned counts[5][4];

JT_TEST(winmem_property)
{
    static struct jw_table before;
    for (unsigned seed = 1; seed <= SEEDS; seed++) {
        ref_init();
        rng = seed * 0x9e3779b97f4a7c15ull;
        for (unsigned step = 0; step < STEPS; step++) {
            uint32_t n = rsize(), at = 0, grow = 0;
            int op = (int)(rnd() % 5u), e = 0, want = 0;
            before = T;
            switch (op) {
            case 0: /* as the runtime: plan, grow, take */
            case 1: {
                int g = ref_plan(n);
                want = n == 0 ? JW_INVALID : nsl >= JW_RUNS - 1u ? JW_FULL : g < 0 ? JW_NONE : JW_OK;
                e = __jw_plan(&T, n, &grow);
                JT_CHECK(e == want && (e != JW_OK || grow == (uint32_t)g));
                if (e != JW_OK) {
                    break;
                }
                e = take_as_runtime(n, &at);
                if (e != JW_OK) {
                    JT_CHECK(e == JW_OK);
                    break;
                }
                JT_CHECK(ref_fit(n) == (int64_t)at);
                for (uint32_t p = at; p < at + n; p++) {
                    JT_CHECK(pg[p] == R_FREE); /* never a page twice */
                }
                ref_take(at, n);
                break;
            }
            case 2: { /* a take alone */
                int64_t f = n == 0 ? -1 : ref_fit(n);
                want = n == 0 ? JW_INVALID : nsl >= JW_RUNS - 1u ? JW_FULL : f < 0 ? JW_NONE : JW_OK;
                e = __jw_take(&T, n, &at);
                JT_CHECK(e == want && (e != JW_OK || at == (uint32_t)f));
                if (e == JW_OK) {
                    ref_take(at, n);
                }
                break;
            }
            case 3: /* a slice out given back */
                if (nsl > 0 && rnd() % 4u != 0) {
                    uint32_t k = (uint32_t)(rnd() % nsl);
                    e = __jw_give(&T, sl[k].page, sl[k].pages);
                    JT_CHECK(e == JW_OK);
                    if (e == JW_OK) {
                        ref_give(k);
                    }
                    break;
                }
                /* Else a range that is free in part (or past the mapped
                 * part, or empty): refused. A range wholly inside slices
                 * isn't one of them: it can't be told from a slice. */
                at = (uint32_t)(rnd() % (r_mapped + 2u));
                n = (uint32_t)(rnd() % 6u);
                {
                    int anyfree = 0;
                    for (uint32_t p = at; p < at + n && p < r_mapped; p++) {
                        anyfree |= pg[p] == R_FREE;
                    }
                    if (n != 0 && at < r_mapped && n <= r_mapped - at && !anyfree) {
                        break; /* inside slices: skipped */
                    }
                }
                e = __jw_give(&T, at, n);
                JT_CHECK(e == JW_INVALID);
                break;
            default: { /* an extent grown without a need, or of a wrong size */
                uint32_t x = __jw_extent_pages(&T);
                uint32_t try = rnd() % 3u == 0 ? x + 1u : x;
                want = try == 0 || try != x ? JW_INVALID : JW_OK;
                e = __jw_grow(&T, try);
                JT_CHECK(e == want);
                if (e == JW_OK) {
                    ref_grow();
                }
                break;
            }
            }
            if (e != JW_OK) {
                JT_CHECK(memcmp(&before, &T, sizeof T) == 0); /* refused: nothing changed */
            }
            counts[op][e >= 0 && e < 4 ? e : 3]++;
            int line = agree();
            if (line != 0) {
                fprintf(stderr, "  seed %u step %u: op %d (result %d): disagrees (test_winmem.c:%d)\n", seed,
                        step, op, e, line);
                JT_CHECK(line == 0);
                return;
            }
        }
    }
    /* Every result of every operation reached. */
    JT_CHECK(counts[0][JW_OK] + counts[1][JW_OK] > 0 && counts[0][JW_INVALID] + counts[1][JW_INVALID] > 0);
    JT_CHECK(counts[0][JW_FULL] + counts[1][JW_FULL] + counts[2][JW_FULL] > 0);
    JT_CHECK(counts[0][JW_NONE] + counts[1][JW_NONE] > 0 && counts[2][JW_NONE] > 0);
    JT_CHECK(counts[3][JW_OK] > 0 && counts[3][JW_INVALID] > 0 && counts[4][JW_OK] > 0 &&
             counts[4][JW_INVALID] > 0);
}
