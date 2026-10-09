/* test_vm.c - page tables (Phase 2 step 4) on a simulated physical memory:
 * unit tests, and a property test against a shadow map of every page. */
#include "harness.h"
#include "vm.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* Simulated physical memory: frames handed out from the bottom, zeroed. The
 * frame numbers are offset so physical address 0 is never a table. */
#define PHYS_FRAMES 512u
#define PHYS_BASE   0x100000ull
static uint8_t phys[PHYS_FRAMES * 4096u] __attribute__((aligned(4096)));
static uint32_t next_frame;
static uint32_t frame_limit;

/* Frames given back (put_frame) are reused first, so long property runs
 * don't run out. */
static uint64_t freed[PHYS_FRAMES];
static uint32_t nfreed;

static kerr get_frame(void *ctx, uint64_t *pa)
{
    (void)ctx;
    if (nfreed != 0) {
        *pa = freed[--nfreed];
        memset(&phys[*pa - PHYS_BASE], 0, 4096u);
        return KERR_OK;
    }
    if (next_frame == frame_limit) {
        return KERR_NOMEM;
    }
    memset(&phys[next_frame * 4096u], 0, 4096u);
    *pa = PHYS_BASE + (uint64_t)next_frame * 4096u;
    next_frame++;
    return KERR_OK;
}

static void *phys_to_ptr(void *ctx, uint64_t pa)
{
    (void)ctx;
    if (pa < PHYS_BASE || pa - PHYS_BASE >= sizeof phys) {
        abort(); /* the code under test followed a pointer outside its tables */
    }
    return &phys[pa - PHYS_BASE];
}

static uint32_t frames_put;

static void put_frame(void *ctx, uint64_t pa)
{
    (void)ctx;
    if (pa < PHYS_BASE || pa - PHYS_BASE >= sizeof phys || pa % 4096u != 0) {
        abort(); /* not a frame this allocator handed out */
    }
    frames_put++;
    freed[nfreed++] = pa;
}

static struct vm_space fresh(void)
{
    struct vm_space s;
    next_frame = 0;
    frame_limit = PHYS_FRAMES;
    frames_put = 0;
    nfreed = 0;
    if (vm_create(&s, get_frame, phys_to_ptr, NULL) != KERR_OK) {
        abort();
    }
    return s;
}

#define UBASE 0x0000000040000000ull /* a user-half address, 1 GiB */

#define KBASE 0xffffffff80000000ull
#define HHDM  0xffff800000000000ull

JT_TEST(vm_maps_and_translates)
{
    struct vm_space s = fresh();
    uint64_t pa;
    unsigned perms;
    JT_REQUIRE(vm_map(&s, KBASE, 0x200000, 0x3000, VM_PAGE, VM_X) == KERR_OK);
    JT_REQUIRE(vm_map(&s, KBASE + 0x3000, 0x203000, 0x1000, VM_PAGE, 0) == KERR_OK);
    JT_REQUIRE(vm_map(&s, KBASE + 0x4000, 0x204000, 0x4000, VM_PAGE, VM_W | VM_GLOBAL) == KERR_OK);
    JT_CHECK(vm_translate(&s, KBASE + 0x1234, &pa, &perms) == KERR_OK && pa == 0x201234 &&
             perms == VM_X);
    JT_CHECK(vm_translate(&s, KBASE + 0x3008, &pa, &perms) == KERR_OK && pa == 0x203008 &&
             perms == 0);
    JT_CHECK(vm_translate(&s, KBASE + 0x7fff, &pa, &perms) == KERR_OK && pa == 0x207fff &&
             perms == (VM_W | VM_GLOBAL));
    JT_CHECK(vm_translate(&s, KBASE + 0x8000, &pa, &perms) == KERR_RANGE);
    /* Device memory: uncached, and it reads back as such. */
    JT_REQUIRE(vm_map(&s, KBASE + 0x10000, 0xfee00000, 0x1000, VM_PAGE, VM_W | VM_UNCACHED) ==
               KERR_OK);
    JT_CHECK(vm_translate(&s, KBASE + 0x10020, &pa, &perms) == KERR_OK && pa == 0xfee00020 &&
             perms == (VM_W | VM_UNCACHED));
    JT_CHECK(vm_translate(&s, 0x1000, &pa, &perms) == KERR_RANGE);
    JT_CHECK(vm_translate(&s, 0x0000800000000000ull, &pa, &perms) ==
             KERR_RANGE); /* non-canonical */
}

JT_TEST(vm_large_pages)
{
    struct vm_space s = fresh();
    uint64_t pa;
    unsigned perms;
    JT_REQUIRE(vm_map(&s, HHDM, 0, 4 * VM_LARGE, VM_LARGE, VM_W) == KERR_OK);
    JT_CHECK(vm_translate(&s, HHDM + 3 * VM_LARGE + 0x12345, &pa, &perms) == KERR_OK &&
             pa == 3 * VM_LARGE + 0x12345 && perms == VM_W);
    /* 4 KiB inside a large page, and a large page over 4 KiB tables: refused. */
    JT_CHECK(vm_map(&s, HHDM + 0x1000, 0x1000, 0x1000, VM_PAGE, 0) == KERR_STATE);
    JT_REQUIRE(vm_map(&s, HHDM + 8 * VM_LARGE, 0, 0x1000, VM_PAGE, 0) == KERR_OK);
    JT_CHECK(vm_map(&s, HHDM + 8 * VM_LARGE, 0, VM_LARGE, VM_LARGE, 0) == KERR_STATE);
    /* A lone large page, and a 4 KiB mapping at an offset whose slot one
     * level up is empty: only the large-page check can refuse it. */
    JT_REQUIRE(vm_map(&s, HHDM + 32 * VM_LARGE, 0, VM_LARGE, VM_LARGE, 0) == KERR_OK);
    JT_CHECK(vm_map(&s, HHDM + 32 * VM_LARGE + 0x5000, 0x5000, 0x1000, VM_PAGE, 0) == KERR_STATE);
    /* Large pages need 2 MiB alignment. */
    JT_CHECK(vm_map(&s, HHDM + 16 * VM_LARGE + 0x1000, 0, VM_LARGE, VM_LARGE, 0) == KERR_INVAL);
}

JT_TEST(vm_refuses_bad_mappings)
{
    struct vm_space s = fresh();
    JT_CHECK(vm_map(&s, KBASE, 0, 0x1000, VM_PAGE, VM_W | VM_X) == KERR_INVAL); /* W^X */
    JT_CHECK(vm_map(&s, KBASE + 1, 0, 0x1000, VM_PAGE, 0) == KERR_INVAL);
    JT_CHECK(vm_map(&s, KBASE, 1, 0x1000, VM_PAGE, 0) == KERR_INVAL);
    JT_CHECK(vm_map(&s, KBASE, 0, 0x800, VM_PAGE, 0) == KERR_INVAL);
    JT_CHECK(vm_map(&s, KBASE, 0, 0, VM_PAGE, 0) == KERR_INVAL);
    JT_CHECK(vm_map(&s, KBASE, 0, 0x1000, 0x8000, 0) == KERR_INVAL); /* not a page size */
    JT_CHECK(vm_map(&s, 0x0000800000000000ull, 0, 0x1000, VM_PAGE, 0) == KERR_INVAL);
    JT_CHECK(vm_map(&s, 0x00007ffffffff000ull, 0, 0x2000, VM_PAGE, 0) ==
             KERR_INVAL); /* runs off the lower half */
    JT_CHECK(vm_map(&s, KBASE, 0x000ffffffffff000ull, 0x2000, VM_PAGE, 0) ==
             KERR_INVAL); /* past 52 bits */
    JT_CHECK(vm_map(&s, 0xfffffffffffff000ull, 0, 0x2000, VM_PAGE, 0) == KERR_INVAL); /* wraps */
    JT_REQUIRE(vm_map(&s, KBASE, 0, 0x2000, VM_PAGE, 0) == KERR_OK);
    JT_CHECK(vm_map(&s, KBASE + 0x1000, 0x5000, 0x1000, VM_PAGE, 0) == KERR_STATE); /* remap */
}

JT_TEST(vm_refused_call_changes_nothing)
{
    struct vm_space s = fresh();
    uint64_t pa;
    unsigned perms;
    JT_REQUIRE(vm_map(&s, KBASE + 0x2000, 0x9000, 0x1000, VM_PAGE, 0) == KERR_OK);
    /* Overlaps the page above at its end: nothing of it may be mapped. */
    JT_CHECK(vm_map(&s, KBASE, 0x1000, 0x3000, VM_PAGE, VM_W) == KERR_STATE);
    JT_CHECK(vm_translate(&s, KBASE, &pa, &perms) == KERR_RANGE);
    JT_CHECK(vm_translate(&s, KBASE + 0x1000, &pa, &perms) == KERR_RANGE);
    JT_CHECK(vm_translate(&s, KBASE + 0x2000, &pa, &perms) == KERR_OK && pa == 0x9000);
}

JT_TEST(vm_out_of_frames)
{
    struct vm_space s = fresh();
    frame_limit = next_frame + 2u; /* not enough for PDPT, PD and PT */
    JT_CHECK(vm_map(&s, KBASE, 0, 0x1000, VM_PAGE, 0) == KERR_NOMEM);
    uint64_t pa;
    unsigned perms;
    JT_CHECK(vm_translate(&s, KBASE, &pa, &perms) == KERR_RANGE);
}

/* ---- property test ---- */

struct shadow_page {
    uint64_t va, pa;
    unsigned perms;
};
static struct shadow_page shadow[4096];
static uint32_t nshadow;
static uint64_t rng_state;

static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

static int shadow_find(uint64_t va)
{
    for (uint32_t i = 0; i < nshadow; i++) {
        if (shadow[i].va == va) {
            return (int)i;
        }
    }
    return -1;
}

JT_TEST(props_vm_matches_shadow)
{
    unsigned long ok = 0, refused = 0;
    for (uint64_t seed = 1; seed <= 200; seed++) {
        struct vm_space s = fresh();
        nshadow = 0;
        rng_state = seed * 0x9E3779B97F4A7C15ULL + 1u;
        /* A small window of the kernel half, so mappings collide often. */
        for (int step = 0; step < 60; step++) {
            uint64_t page = rng() % 64u;
            uint64_t npages = 1u + rng() % 4u;
            uint64_t va = KBASE + page * VM_PAGE;
            uint64_t pa = (rng() % 4096u) * VM_PAGE;
            unsigned perms = (unsigned)(rng() % 32u);
            if (rng() % 8u == 0) {
                va += 8; /* misaligned now and then */
            }
            kerr want = KERR_OK;
            if (va % VM_PAGE != 0 || ((perms & VM_W) && (perms & VM_X))) {
                want = KERR_INVAL;
            } else {
                for (uint64_t i = 0; i < npages; i++) {
                    if (shadow_find(va + i * VM_PAGE) >= 0) {
                        want = KERR_STATE;
                    }
                }
            }
            kerr got = vm_map(&s, va, pa, npages * VM_PAGE, VM_PAGE, perms);
            if (got != want) {
                fprintf(stderr, "  seed %" PRIu64 " step %d: map returned %d, shadow %d\n", seed,
                        step, got, want);
                JT_CHECK(0);
                return;
            }
            if (got == KERR_OK) {
                ok++;
                for (uint64_t i = 0; i < npages; i++) {
                    shadow[nshadow++] =
                        (struct shadow_page){va + i * VM_PAGE, pa + i * VM_PAGE, perms};
                }
            } else {
                refused++;
            }
            /* Every page in the window translates exactly as the shadow says. */
            for (uint64_t p = 0; p < 68u; p++) {
                uint64_t v = KBASE + p * VM_PAGE;
                uint64_t gpa;
                unsigned gperms;
                kerr t = vm_translate(&s, v + 0x123, &gpa, &gperms);
                int i = shadow_find(v);
                if (i < 0 ? t != KERR_RANGE
                          : (t != KERR_OK || gpa != shadow[i].pa + 0x123 ||
                             gperms != shadow[i].perms)) {
                    fprintf(stderr, "  seed %" PRIu64 " step %d: page %" PRIu64 " differs\n", seed,
                            step, p);
                    JT_CHECK(0);
                    return;
                }
            }
        }
    }
    fprintf(stderr, "  coverage maps ok=%lu refused=%lu\n", ok, refused);
    JT_CHECK(ok > 1000 && refused > 1000);
}

JT_TEST(vm_unmap_frees_emptied_tables)
{
    struct vm_space s = fresh();
    s.put_frame = put_frame;
    uint64_t pa;
    unsigned perms;
    /* Two pages in one page table, one in another 2 MiB away. */
    JT_REQUIRE(vm_map(&s, UBASE, 0x10000, 0x2000, VM_PAGE, VM_USER) == KERR_OK);
    JT_REQUIRE(vm_map(&s, UBASE + VM_LARGE, 0x20000, 0x1000, VM_PAGE, VM_USER) == KERR_OK);
    uint32_t taken = next_frame; /* PML4, PDPT, PD, two PTs */
    JT_CHECK(taken == 5u);
    JT_REQUIRE(vm_unmap(&s, UBASE, 0x1000) == KERR_OK);
    JT_CHECK(frames_put == 0); /* its page table still maps UBASE + 0x1000 */
    JT_CHECK(vm_translate(&s, UBASE, &pa, &perms) == KERR_RANGE);
    JT_CHECK(vm_translate(&s, UBASE + 0x1000, &pa, &perms) == KERR_OK && pa == 0x11000);
    JT_REQUIRE(vm_unmap(&s, UBASE + 0x1000, 0x1000) == KERR_OK);
    JT_CHECK(frames_put == 1); /* that PT, now empty */
    JT_REQUIRE(vm_unmap(&s, UBASE + VM_LARGE, 0x1000) == KERR_OK);
    JT_CHECK(frames_put == 4); /* the other PT, the PD, the PDPT: only the PML4 is left */
    JT_CHECK(vm_translate(&s, UBASE + VM_LARGE, &pa, &perms) == KERR_RANGE);
    /* Mapping again builds the tables again. */
    JT_REQUIRE(vm_map(&s, UBASE, 0x10000, 0x1000, VM_PAGE, VM_USER) == KERR_OK);
    JT_CHECK(vm_translate(&s, UBASE, &pa, &perms) == KERR_OK && pa == 0x10000);
}

JT_TEST(vm_unmap_refusals_change_nothing)
{
    struct vm_space s = fresh();
    s.put_frame = put_frame;
    uint64_t pa;
    unsigned perms;
    JT_REQUIRE(vm_map(&s, UBASE, 0x10000, 0x2000, VM_PAGE, VM_USER) == KERR_OK);
    /* The range runs one page past the mapping: nothing is unmapped. */
    JT_CHECK(vm_unmap(&s, UBASE, 0x3000) == KERR_RANGE);
    JT_CHECK(vm_translate(&s, UBASE, &pa, &perms) == KERR_OK && frames_put == 0);
    JT_CHECK(vm_unmap(&s, UBASE + 8, 0x1000) == KERR_INVAL);
    JT_CHECK(vm_unmap(&s, UBASE, 0) == KERR_INVAL);
    JT_CHECK(vm_unmap(&s, 0x0000800000000000ull, 0x1000) == KERR_INVAL); /* non-canonical */
    /* A large page isn't unmapped page by page. */
    JT_REQUIRE(vm_map(&s, HHDM, 0, VM_LARGE, VM_LARGE, VM_W) == KERR_OK);
    JT_CHECK(vm_unmap(&s, HHDM, 0x1000) == KERR_RANGE);
}

JT_TEST(vm_upper_half_tables_are_never_freed)
{
    struct vm_space s = fresh();
    s.put_frame = put_frame;
    JT_REQUIRE(vm_map(&s, KBASE, 0x10000, 0x1000, VM_PAGE, 0) == KERR_OK);
    JT_REQUIRE(vm_unmap(&s, KBASE, 0x1000) == KERR_OK);
    JT_CHECK(frames_put == 0); /* shared by every space: kept */
}

JT_TEST(vm_destroy_returns_every_table)
{
    struct vm_space s = fresh();
    s.put_frame = put_frame;
    JT_REQUIRE(vm_map(&s, UBASE, 0x10000, 0x3000, VM_PAGE, VM_USER) == KERR_OK);
    JT_REQUIRE(vm_map(&s, UBASE + 5u * VM_LARGE, 0x20000, 0x1000, VM_PAGE, VM_USER) == KERR_OK);
    JT_REQUIRE(vm_map(&s, 0x0000004000000000ull, 0x30000, 0x1000, VM_PAGE, VM_USER) == KERR_OK);
    vm_destroy(&s);
    JT_CHECK(frames_put == next_frame); /* every table, the PML4 included */
}

/* Tables needed for a set of mapped user pages: the PML4, and one table per
 * distinct 512 GiB, 1 GiB and 2 MiB region holding a mapped page. */
static uint32_t tables_needed(const uint8_t *mapped, const uint64_t *va, uint32_t n)
{
    uint32_t count = 1;
    for (unsigned level = 1; level <= 3u; level++) {
        unsigned shift = 12u + 9u * level;
        for (uint32_t i = 0; i < n; i++) {
            if (!mapped[i]) {
                continue;
            }
            int first = 1;
            for (uint32_t j = 0; j < i; j++) {
                if (mapped[j] && (va[j] >> shift) == (va[i] >> shift)) {
                    first = 0;
                    break;
                }
            }
            count += (uint32_t)first;
        }
    }
    return count;
}

JT_TEST(props_vm_unmap_reclaims_exactly)
{
    /* 48 pages: 16 below and 16 above a 2 MiB boundary, and 16 above a
     * 1 GiB boundary, so PTs, PDs and PDPT-level splits are all exercised. */
    enum { N = 48 };
    uint64_t va[N];
    for (uint32_t i = 0; i < 16u; i++) {
        va[i] = UBASE + VM_LARGE - 16u * VM_PAGE + i * VM_PAGE;
        va[16u + i] = UBASE + VM_LARGE + i * VM_PAGE;
        va[32u + i] = UBASE + 0x40000000ull + i * VM_PAGE;
    }
    unsigned long maps = 0, unmaps = 0;
    for (uint64_t seed = 1; seed <= 100; seed++) {
        struct vm_space s = fresh();
        s.put_frame = put_frame;
        uint8_t mapped[N] = {0};
        rng_state = seed * 0x9E3779B97F4A7C15ULL + 7u;
        for (int step = 0; step < 300; step++) {
            uint32_t i = (uint32_t)(rng() % N);
            uint32_t run = 1u + (uint32_t)(rng() % 4u);
            if (i + run > N || (i % 16u) + run > 16u) {
                run = 1; /* keep runs inside one group of contiguous pages */
            }
            int all_mapped = 1;
            int none_mapped = 1;
            for (uint32_t j = i; j < i + run; j++) {
                all_mapped &= mapped[j];
                none_mapped &= !mapped[j];
            }
            if (rng() % 2u == 0) {
                kerr e = vm_map(&s, va[i], 0x100000u + i * VM_PAGE, run * VM_PAGE, VM_PAGE, VM_USER);
                if ((e == KERR_OK) != none_mapped) {
                    JT_CHECK(0 && "map result differs from the shadow");
                    return;
                }
                if (e == KERR_OK) {
                    maps++;
                    for (uint32_t j = i; j < i + run; j++) {
                        mapped[j] = 1;
                    }
                }
            } else {
                kerr e = vm_unmap(&s, va[i], run * VM_PAGE);
                if ((e == KERR_OK) != all_mapped) {
                    JT_CHECK(0 && "unmap result differs from the shadow");
                    return;
                }
                if (e == KERR_OK) {
                    unmaps++;
                    for (uint32_t j = i; j < i + run; j++) {
                        mapped[j] = 0;
                    }
                }
            }
            /* Translations match the shadow, and exactly the needed tables
             * are in use: none leaked, none freed too early. (Every run stays
             * inside one page table, so a refused map only walks tables that
             * already exist and leaves nothing behind.) */
            for (uint32_t j = 0; j < N; j++) {
                uint64_t pa;
                unsigned perms;
                kerr t = vm_translate(&s, va[j], &pa, &perms);
                if ((t == KERR_OK) != (mapped[j] != 0)) {
                    JT_CHECK(0 && "translation differs from the shadow");
                    return;
                }
            }
            uint32_t in_use = next_frame - nfreed;
            uint32_t need = tables_needed(mapped, va, N);
            if (in_use != need) {
                fprintf(stderr, "  seed %llu step %d: %u tables in use, %u needed\n",
                        (unsigned long long)seed, step, in_use, need);
                JT_CHECK(0);
                return;
            }
        }
        /* vm_destroy returns every table there is. */
        vm_destroy(&s);
        if (next_frame != nfreed) {
            fprintf(stderr, "  seed %llu: %u tables never returned\n", (unsigned long long)seed,
                    next_frame - nfreed);
            JT_CHECK(0);
            return;
        }
    }
    fprintf(stderr, "  coverage unmap maps=%lu unmaps=%lu\n", maps, unmaps);
    JT_CHECK(maps > 1000 && unmaps > 1000);
}

JT_TEST(vm_shared_upper_half)
{
    /* After prefilling, a kernel mapping made later shows in a space that
     * copied the upper half before it was made. */
    struct vm_space kern = fresh();
    JT_REQUIRE(vm_prefill_upper(&kern) == KERR_OK);
    JT_CHECK(next_frame == 257u); /* the PML4 and 256 PDPTs */
    struct vm_space task;
    JT_REQUIRE(vm_create(&task, get_frame, phys_to_ptr, NULL) == KERR_OK);
    vm_share_upper(&task, &kern);
    JT_REQUIRE(vm_map(&kern, KBASE + 0x5000, 0x77000, 0x1000, VM_PAGE, VM_W) == KERR_OK);
    uint64_t pa;
    unsigned perms;
    JT_CHECK(vm_translate(&task, KBASE + 0x5000, &pa, &perms) == KERR_OK && pa == 0x77000);
    JT_CHECK((perms & VM_USER) == 0); /* never user-accessible, at any level */
    /* Prefilling twice changes nothing. */
    uint32_t before = next_frame;
    JT_REQUIRE(vm_prefill_upper(&kern) == KERR_OK);
    JT_CHECK(next_frame == before);
}

