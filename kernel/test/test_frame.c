/* SPDX-License-Identifier: GPL-3.0-only */
/* test_frame.c - the boot-time frame allocator (Phase 2 step 3): unit tests
 * and a property test against a plain shadow, as in model/. */
#include "frame.h"
#include "harness.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define MiB (1024u * 1024u)

static uint64_t store[4096]; /* 8-byte aligned storage for the bitmaps */

/* ---- unit tests ---- */

JT_TEST(frame_init_rejects_malformed_maps)
{
    struct frame_alloc fa;
    struct frame_range unsorted[] = {{2 * MiB, MiB}, {0, MiB}};
    struct frame_range overlap[] = {{0, 2 * MiB}, {MiB, MiB}};
    struct frame_range empty[] = {{MiB, 0}};
    /* A valid range, then one that wraps: the whole map is refused, not just
     * the bad range ignored. */
    struct frame_range wraps[] = {{0, MiB}, {UINT64_MAX - 100, 200}};
    struct frame_range tiny[] = {{100, 200}}; /* no whole frame */
    struct frame_range ok[] = {{0, MiB}};
    JT_CHECK(frame_init(&fa, unsorted, 2, store, sizeof store) == KERR_INVAL);
    JT_CHECK(frame_init(&fa, overlap, 2, store, sizeof store) == KERR_INVAL);
    JT_CHECK(frame_init(&fa, empty, 1, store, sizeof store) == KERR_INVAL);
    JT_CHECK(frame_init(&fa, wraps, 2, store, sizeof store) == KERR_INVAL);
    JT_CHECK(frame_init(&fa, tiny, 1, store, sizeof store) == KERR_INVAL);
    JT_CHECK(frame_init(&fa, NULL, 1, store, sizeof store) == KERR_INVAL);
    JT_CHECK(frame_init(&fa, ok, 0, store, sizeof store) == KERR_INVAL);
    /* Storage: too small, or misaligned. */
    JT_CHECK(frame_bitmap_bytes(ok, 1) == 2u * 4u * 8u); /* 256 frames: 4 words, twice */
    JT_CHECK(frame_init(&fa, ok, 1, store, frame_bitmap_bytes(ok, 1) - 1u) == KERR_INVAL);
    JT_CHECK(frame_init(&fa, ok, 1, (char *)store + 4, sizeof store - 8) == KERR_INVAL);
    JT_CHECK(frame_init(&fa, ok, 1, store, sizeof store) == KERR_OK);
    JT_CHECK(fa.nfree == 256 && frame_check(&fa));
}

JT_TEST(frame_ranges_are_trimmed_inwards)
{
    struct frame_alloc fa;
    /* 0x1800..0x5800: frames 2, 3, 4 (0x1800 rounds up to 0x2000, 0x5800
     * down to 0x5000). */
    struct frame_range r[] = {{0x1800, 0x4000}};
    JT_REQUIRE(frame_init(&fa, r, 1, store, sizeof store) == KERR_OK);
    JT_CHECK(fa.base_pfn == 2 && fa.nframes == 3 && fa.nfree == 3);
    uint64_t p[3];
    for (int i = 0; i < 3; i++) {
        JT_REQUIRE(frame_get(&fa, &p[i]) == KERR_OK);
        JT_CHECK(p[i] >= 0x2000 && p[i] < 0x5000 && p[i] % FRAME_SIZE == 0);
    }
    uint64_t x;
    JT_CHECK(frame_get(&fa, &x) == KERR_NOMEM);
    JT_CHECK(frame_check(&fa));
}

JT_TEST(frame_gaps_are_never_handed_out_or_freed)
{
    struct frame_alloc fa;
    /* Frames 0-1 and 4-5 usable; 2-3 a gap. */
    struct frame_range r[] = {{0, 0x2000}, {0x4000, 0x2000}};
    JT_REQUIRE(frame_init(&fa, r, 2, store, sizeof store) == KERR_OK);
    JT_CHECK(fa.nframes == 6 && fa.nfree == 4);
    uint64_t p;
    for (int i = 0; i < 4; i++) {
        JT_REQUIRE(frame_get(&fa, &p) == KERR_OK);
        JT_CHECK(p != 0x2000 && p != 0x3000);
    }
    JT_CHECK(frame_get(&fa, &p) == KERR_NOMEM);
    /* A gap frame was never handed out, so it can't be freed. */
    JT_CHECK(frame_put(&fa, 0x2000) == KERR_STATE);
    JT_CHECK(fa.nfree == 0 && frame_check(&fa));
}

JT_TEST(frame_put_errors)
{
    struct frame_alloc fa;
    struct frame_range r[] = {{0x10000, 0x4000}};
    JT_REQUIRE(frame_init(&fa, r, 1, store, sizeof store) == KERR_OK);
    uint64_t p;
    JT_REQUIRE(frame_get(&fa, &p) == KERR_OK);
    JT_CHECK(frame_put(&fa, p + 8) == KERR_INVAL);
    JT_CHECK(frame_put(&fa, 0x0) == KERR_RANGE);
    JT_CHECK(frame_put(&fa, 0x14000) == KERR_RANGE);
    JT_CHECK(frame_put(&fa, p) == KERR_OK);
    JT_CHECK(frame_put(&fa, p) == KERR_STATE); /* double free */
    JT_CHECK(fa.nfree == 4 && frame_check(&fa));
}

JT_TEST(frame_reserve_is_permanent)
{
    struct frame_alloc fa;
    struct frame_range r[] = {{0, 0x8000}};
    JT_REQUIRE(frame_init(&fa, r, 1, store, sizeof store) == KERR_OK);
    /* 0x1800..0x3800 rounds outwards to frames 1, 2, 3. */
    frame_reserve(&fa, 0x1800, 0x2000);
    JT_CHECK(fa.nfree == 5);
    frame_reserve(&fa, 0x1000, 0x1000); /* already reserved: no change */
    JT_CHECK(fa.nfree == 5);
    frame_reserve(&fa, 0x7000, 0x10000); /* partly outside: the inside part */
    JT_CHECK(fa.nfree == 4);
    frame_reserve(&fa, UINT64_MAX - 10, 100); /* entirely outside, and wraps */
    frame_reserve(&fa, 0x4000, 0);            /* empty */
    JT_CHECK(fa.nfree == 4);
    JT_CHECK(frame_put(&fa, 0x2000) == KERR_STATE); /* the kernel image can't be freed */
    uint64_t p;
    for (int i = 0; i < 4; i++) {
        JT_REQUIRE(frame_get(&fa, &p) == KERR_OK);
        JT_CHECK(p == 0 || (p >= 0x4000 && p < 0x7000));
    }
    JT_CHECK(frame_get(&fa, &p) == KERR_NOMEM && frame_check(&fa));
}

JT_TEST(frame_take_range_all_or_nothing)
{
    struct frame_alloc fa;
    struct frame_range r[] = {{0, MiB}, {2 * MiB, MiB}}; /* a 1 MiB gap between */
    JT_REQUIRE(frame_init(&fa, r, 2, store, sizeof store) == KERR_OK);
    uint64_t before = fa.nfree;
    JT_CHECK(frame_take_range(&fa, 0x10000, 0x4000) == KERR_OK);
    JT_CHECK(fa.nfree == before - 4u && frame_check(&fa));
    JT_CHECK(frame_put(&fa, 0x10000) == KERR_STATE); /* gone for good */
    /* Overlapping taken frames, or the gap: refused, nothing taken. */
    JT_CHECK(frame_take_range(&fa, 0xe000, 0x4000) == KERR_STATE);
    JT_CHECK(frame_take_range(&fa, MiB - 0x2000, 0x4000) == KERR_STATE); /* runs into the gap */
    JT_CHECK(fa.nfree == before - 4u && frame_check(&fa));
    JT_CHECK(frame_take_range(&fa, 0x100, 0x1000) == KERR_INVAL);
    JT_CHECK(frame_take_range(&fa, 0x1000, 0) == KERR_INVAL);
    JT_CHECK(frame_take_range(&fa, 3 * MiB, 0x1000) == KERR_RANGE); /* past the end */
    /* A frame handed out by frame_get blocks a range over it. */
    uint64_t pa;
    JT_REQUIRE(frame_get(&fa, &pa) == KERR_OK);
    JT_CHECK(frame_take_range(&fa, pa, 0x1000) == KERR_STATE);
    JT_CHECK(frame_check(&fa));
}

JT_TEST(frame_large_map_bounds)
{
    struct frame_alloc fa;
    /* 64 MiB = 16384 frames: 256 words per bitmap, 4 KiB of storage. */
    struct frame_range r[] = {{0, 64u * MiB}};
    JT_REQUIRE(frame_bitmap_bytes(r, 1) == 4096u);
    JT_REQUIRE(frame_init(&fa, r, 1, store, sizeof store) == KERR_OK);
    uint64_t p;
    uint64_t n = 0;
    while (frame_get(&fa, &p) == KERR_OK) {
        n++;
    }
    JT_CHECK(n == 16384 && fa.nfree == 0 && frame_check(&fa));
}

/* ---- property test ---- */

enum { NEVER, FREE, TAKEN };
#define PFRAMES 300u
static uint8_t sh[PFRAMES]; /* shadow, by absolute frame number */
static uint64_t rng_state;
/* Coverage: results of get (OK, NOMEM) and put (by kerr), so a generator
 * that never reaches a path is visible. */
static unsigned long cov_get[2], cov_put[5];

static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static uint32_t pick(uint32_t n)
{
    return (uint32_t)(rng() % n);
}

static int compare(const struct frame_alloc *fa, char *why, size_t len)
{
    uint64_t nfree = 0;
    for (uint32_t f = 0; f < PFRAMES; f++) {
        nfree += sh[f] == FREE;
        int inside = f >= fa->base_pfn && f - fa->base_pfn < fa->nframes;
        if (!inside && sh[f] != NEVER) {
            (void)snprintf(why, len, "frame %u: shadow %u outside the covered span", f, sh[f]);
            return 0;
        }
    }
    if (nfree != fa->nfree || !frame_check(fa)) {
        (void)snprintf(why, len, "nfree %" PRIu64 ", shadow %" PRIu64 ", check %d", fa->nfree,
                       nfree, frame_check(fa));
        return 0;
    }
    return 1;
}

static int run_seed(uint64_t seed)
{
    char why[160];
    struct frame_alloc fa;
    struct frame_range r[8];
    size_t n = 0;
    rng_state = seed * 0x9E3779B97F4A7C15ULL + 1u;
    memset(sh, NEVER, sizeof sh);
    /* A random sorted, non-overlapping map with unaligned edges. */
    uint64_t at = (uint64_t)pick(3 * FRAME_SIZE);
    while (n < 8 && at < (PFRAMES - 40u) * FRAME_SIZE) {
        uint64_t len = 1u + (uint64_t)pick(30 * FRAME_SIZE);
        r[n].base = at;
        r[n].length = len;
        for (uint64_t f = (at + FRAME_SIZE - 1u) / FRAME_SIZE; f < (at + len) / FRAME_SIZE; f++) {
            sh[f] = FREE;
        }
        n++;
        at += len + (uint64_t)pick(8 * FRAME_SIZE);
    }
    kerr e = frame_init(&fa, r, n, store, sizeof store);
    int any = 0;
    for (uint32_t f = 0; f < PFRAMES; f++) {
        any |= sh[f] == FREE;
    }
    if ((e == KERR_OK) != any) {
        fprintf(stderr, "  seed %" PRIu64 ": init %d with %d usable frames\n", seed, e, any);
        return 0;
    }
    if (e != KERR_OK) {
        return 1;
    }
    /* A reservation at boot, as for the kernel image. */
    uint64_t rb = (uint64_t)pick(PFRAMES * FRAME_SIZE);
    uint64_t rl = (uint64_t)pick(20 * FRAME_SIZE);
    frame_reserve(&fa, rb, rl);
    if (rl != 0) {
        for (uint64_t f = rb / FRAME_SIZE;
             f < (rb + rl + FRAME_SIZE - 1u) / FRAME_SIZE && f < PFRAMES; f++) {
            if (sh[f] != NEVER) {
                sh[f] = NEVER;
            }
        }
    }
    /* Reserved frames inside the span stay covered but are never free. */
    uint64_t held[PFRAMES];
    uint32_t nheld = 0;
    for (int step = 0; step < 2000; step++) {
        if (pick(2) == 0) {
            uint64_t p = 0;
            kerr g = frame_get(&fa, &p);
            int any_free = 0;
            for (uint32_t f = 0; f < PFRAMES; f++) {
                any_free |= sh[f] == FREE;
            }
            cov_get[g == KERR_OK ? 0 : 1]++;
            if (g == KERR_OK) {
                uint64_t f = p / FRAME_SIZE;
                if (p % FRAME_SIZE != 0 || f >= PFRAMES || sh[f] != FREE) {
                    fprintf(stderr,
                            "  seed %" PRIu64 " step %d: got frame %" PRIu64 " (shadow %u)\n", seed,
                            step, f, f < PFRAMES ? sh[f] : 9);
                    return 0;
                }
                sh[f] = TAKEN;
                held[nheld++] = p;
            } else if (g != KERR_NOMEM || any_free) {
                fprintf(stderr, "  seed %" PRIu64 " step %d: get %d with free frames %d\n", seed,
                        step, g, any_free);
                return 0;
            }
        } else {
            /* Put back a held frame, or try a bad address. */
            uint64_t p;
            int legit = nheld != 0 && pick(4) != 0;
            uint32_t idx = 0;
            if (legit) {
                idx = pick(nheld);
                p = held[idx];
            } else {
                p = (uint64_t)pick((PFRAMES + 4u) * FRAME_SIZE);
                if (pick(2) == 0) {
                    p -= p % FRAME_SIZE;
                }
            }
            kerr want;
            uint64_t f = p / FRAME_SIZE;
            if (p % FRAME_SIZE != 0) {
                want = KERR_INVAL;
            } else if (f < fa.base_pfn || f - fa.base_pfn >= fa.nframes) {
                want = KERR_RANGE;
            } else if (f >= PFRAMES || sh[f] != TAKEN) {
                want = KERR_STATE;
            } else {
                want = KERR_OK;
            }
            kerr got = frame_put(&fa, p);
            cov_put[(unsigned)got < 5u ? (unsigned)got : 4u]++;
            if (got != want) {
                fprintf(stderr,
                        "  seed %" PRIu64 " step %d: put %" PRIx64 " returned %d, shadow %d\n",
                        seed, step, p, got, want);
                return 0;
            }
            if (got == KERR_OK) {
                sh[f] = FREE;
                for (uint32_t i = 0; i < nheld; i++) {
                    if (held[i] == p) {
                        held[i] = held[--nheld];
                        break;
                    }
                }
            }
        }
        if (!compare(&fa, why, sizeof why)) {
            fprintf(stderr, "  seed %" PRIu64 " step %d: %s\n", seed, step, why);
            return 0;
        }
    }
    return 1;
}

JT_TEST(props_frame_allocator_matches_shadow)
{
    const char *one = getenv("JOIST_PROP_SEED");
    if (one != NULL && *one != '\0') {
        JT_CHECK(run_seed(strtoull(one, NULL, 0)));
        return;
    }
    for (uint64_t s = 1; s <= 500; s++) {
        if (!run_seed(s)) {
            JT_CHECK(0 && "property failed; replay with JOIST_PROP_SEED");
            return;
        }
    }
    fprintf(stderr,
            "  coverage get OK=%lu NOMEM=%lu; put OK=%lu INVAL=%lu NOMEM=%lu RANGE=%lu STATE=%lu\n",
            cov_get[0], cov_get[1], cov_put[KERR_OK], cov_put[KERR_INVAL], cov_put[KERR_NOMEM],
            cov_put[KERR_RANGE], cov_put[KERR_STATE]);
    /* Every path reached, or the test isn't testing it. */
    JT_CHECK(cov_get[0] && cov_get[1] && cov_put[KERR_OK] && cov_put[KERR_INVAL] &&
             cov_put[KERR_RANGE] && cov_put[KERR_STATE]);
}
