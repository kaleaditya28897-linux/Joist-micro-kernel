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

static kerr get_frame(void *ctx, uint64_t *pa)
{
    (void)ctx;
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

static struct vm_space fresh(void)
{
    struct vm_space s;
    next_frame = 0;
    frame_limit = PHYS_FRAMES;
    if (vm_create(&s, get_frame, phys_to_ptr, NULL) != KERR_OK) {
        abort();
    }
    return s;
}

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
            unsigned perms = (unsigned)(rng() % 16u);
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
