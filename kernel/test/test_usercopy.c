/* SPDX-License-Identifier: GPL-3.0-only */
/* test_usercopy.c - copy_from_user (Phase 2 step 6) and copy_to_user (Phase
 * 3 step 6) over simulated physical memory: unit tests and a property test against a shadow of the mappings. */
#include "harness.h"
#include "usercopy.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define PFRAMES 256u
#define PBASE   0x200000ull
static uint8_t mem[PFRAMES * 4096u] __attribute__((aligned(4096)));
static uint32_t next;

static kerr get_frame(void *ctx, uint64_t *pa)
{
    (void)ctx;
    if (next == PFRAMES) {
        return KERR_NOMEM;
    }
    memset(&mem[next * 4096u], 0, 4096u);
    *pa = PBASE + (uint64_t)next * 4096u;
    next++;
    return KERR_OK;
}

static void *to_ptr(void *ctx, uint64_t pa)
{
    (void)ctx;
    if (pa < PBASE || pa - PBASE >= sizeof mem) {
        abort(); /* copy_from_user followed an address it shouldn't have */
    }
    return &mem[pa - PBASE];
}

/* A space with user pages 0x400000.. (16 pages): page i is backed by a
 * frame whose bytes are (i * 7 + offset) & 0xff; pages 5 and 11 are mapped
 * without VM_USER (kernel-only), page 8 not at all, page 13 read-only. */
#define UBASE 0x400000ull
static uint64_t frame_of[16];

static struct vm_space space(void)
{
    struct vm_space s;
    next = 0;
    if (vm_create(&s, get_frame, to_ptr, NULL) != KERR_OK) {
        abort();
    }
    for (unsigned i = 0; i < 16u; i++) {
        if (i == 8u) {
            continue;
        }
        uint64_t pa;
        if (get_frame(NULL, &pa) != KERR_OK) {
            abort();
        }
        uint8_t *p = to_ptr(NULL, pa);
        for (unsigned b = 0; b < 4096u; b++) {
            p[b] = (uint8_t)(i * 7u + b);
        }
        frame_of[i] = pa;
        unsigned perms = (i == 5u || i == 11u) ? VM_W : i == 13u ? VM_USER : (VM_W | VM_USER);
        if (vm_map(&s, UBASE + i * 4096u, pa, 4096u, VM_PAGE, perms) != KERR_OK) {
            abort();
        }
    }
    /* Something in the kernel half, mapped user-accessible and writable on purpose: a
     * pointer there must still be refused by the half check. */
    uint64_t kpa;
    if (get_frame(NULL, &kpa) != KERR_OK ||
        vm_map(&s, 0xffff800000000000ull, kpa, 4096u, VM_PAGE, VM_USER | VM_W) != KERR_OK) {
        abort();
    }
    return s;
}

static int byte_ok(uint64_t va, uint8_t got)
{
    uint64_t i = (va - UBASE) / 4096u;
    uint64_t b = (va - UBASE) % 4096u;
    return got == (uint8_t)(i * 7u + b);
}

JT_TEST(usercopy_reads_across_pages)
{
    struct vm_space s = space();
    static uint8_t buf[3 * 4096u];
    JT_REQUIRE(copy_from_user(&s, buf, UBASE + 0x0ff0, 0x2020) == KERR_OK); /* pages 0..2 */
    for (uint64_t k = 0; k < 0x2020; k++) {
        JT_CHECK(byte_ok(UBASE + 0x0ff0 + k, buf[k]));
    }
}

JT_TEST(usercopy_refuses)
{
    struct vm_space s = space();
    static uint8_t buf[8192];
    JT_CHECK(copy_from_user(&s, buf, UBASE + 5 * 4096u, 1) == KERR_RANGE); /* kernel-only page */
    JT_CHECK(copy_from_user(&s, buf, UBASE + 4 * 4096u + 4000, 200) ==
             KERR_RANGE);                                                      /* runs into it */
    JT_CHECK(copy_from_user(&s, buf, UBASE + 8 * 4096u, 1) == KERR_RANGE);     /* unmapped */
    JT_CHECK(copy_from_user(&s, buf, 0x1000, 1) == KERR_RANGE);                /* nothing there */
    JT_CHECK(copy_from_user(&s, buf, 0xffff800000000000ull, 1) == KERR_RANGE); /* kernel half */
    JT_CHECK(copy_from_user(&s, buf, USER_TOP - 1u, 2) == KERR_RANGE);         /* crosses the top */
    JT_CHECK(copy_from_user(&s, buf, UBASE, SIZE_MAX) == KERR_RANGE);          /* wraps */
    JT_CHECK(copy_from_user(&s, buf, 0xffffffffffffffffull, 0) ==
             KERR_OK); /* empty: nothing read */
}

JT_TEST(props_usercopy_matches_shadow)
{
    static uint8_t buf[6 * 4096u];
    struct vm_space s = space();
    uint64_t r = 0x12345;
    unsigned long ok = 0, refused = 0;
    for (int iter = 0; iter < 100000; iter++) {
        r ^= r >> 12;
        r ^= r << 25;
        r ^= r >> 27;
        uint64_t x = r * 0x2545F4914F6CDD1DULL;
        uint64_t va = UBASE - 4096u + x % (18u * 4096u); /* a page either side too */
        size_t len = (size_t)((x >> 32) % (5u * 4096u));
        int want_ok = 1;
        /* Validity is per page: every page the range touches must be a user page. */
        for (uint64_t a = va; len != 0 && a < va + len && want_ok; a = (a | 4095u) + 1u) {
            uint64_t i = (a - UBASE) / 4096u;
            if (a < UBASE || i >= 16u || i == 5u || i == 8u || i == 11u) {
                want_ok = 0;
            }
        }
        kerr got = copy_from_user(&s, buf, va, len);
        if ((got == KERR_OK) != want_ok || (got != KERR_OK && got != KERR_RANGE)) {
            fprintf(stderr, "  iteration %d: va %" PRIx64 " len %zu: got %d, want %s\n", iter, va,
                    len, got, want_ok ? "OK" : "RANGE");
            JT_CHECK(0);
            return;
        }
        if (got == KERR_OK) {
            ok++;
            for (size_t k = 0; k < len; k++) {
                if (!byte_ok(va + k, buf[k])) {
                    fprintf(stderr, "  iteration %d: byte %zu wrong\n", iter, k);
                    JT_CHECK(0);
                    return;
                }
            }
        } else {
            refused++;
        }
    }
    fprintf(stderr, "  coverage usercopy ok=%lu refused=%lu\n", ok, refused);
    JT_CHECK(ok > 1000 && refused > 1000);
}

/* ---- copy_to_user (Phase 3 step 6) ---- */

JT_TEST(usercopy_writes_across_pages)
{
    static uint8_t src[0x2020];
    static uint8_t back[0x2020];
    struct vm_space s = space();
    for (size_t i = 0; i < sizeof src; i++) {
        src[i] = (uint8_t)(i * 13u + 1u);
    }
    JT_REQUIRE(copy_to_user(&s, UBASE + 0x0ff0, src, sizeof src) == KERR_OK); /* pages 0..3 */
    JT_REQUIRE(copy_from_user(&s, back, UBASE + 0x0ff0, sizeof back) == KERR_OK);
    JT_CHECK(memcmp(src, back, sizeof src) == 0);
    /* The bytes either side are as they were. */
    uint8_t edge[2];
    JT_REQUIRE(copy_from_user(&s, &edge[0], UBASE + 0x0fef, 1) == KERR_OK);
    JT_REQUIRE(copy_from_user(&s, &edge[1], UBASE + 0x0ff0 + sizeof src, 1) == KERR_OK);
    JT_CHECK(byte_ok(UBASE + 0x0fef, edge[0]) && byte_ok(UBASE + 0x0ff0 + sizeof src, edge[1]));
}

JT_TEST(usercopy_write_refuses_and_writes_nothing)
{
    static uint8_t src[2 * 4096u];
    uint8_t b;
    struct vm_space s = space();
    memset(src, 0xa5, sizeof src);
    JT_CHECK(copy_to_user(&s, UBASE + 13 * 4096u, src, 1) == KERR_RANGE); /* read-only */
    JT_CHECK(copy_to_user(&s, UBASE + 5 * 4096u, src, 1) == KERR_RANGE);  /* kernel-only */
    JT_CHECK(copy_to_user(&s, UBASE + 8 * 4096u, src, 1) == KERR_RANGE);  /* unmapped */
    JT_CHECK(copy_to_user(&s, 0xffff800000000000ull, src, 1) == KERR_RANGE);
    JT_CHECK(copy_to_user(&s, USER_TOP - 1u, src, 2) == KERR_RANGE);
    JT_CHECK(copy_to_user(&s, UBASE, src, SIZE_MAX) == KERR_RANGE);
    JT_CHECK(copy_to_user(&s, 0xffffffffffffffffull, src, 0) == KERR_OK);
    /* A range that ends on a page it may not write: none of it is written,
     * not even the part on the page before. */
    JT_CHECK(copy_to_user(&s, UBASE + 12 * 4096u + 4000u, src, 200) == KERR_RANGE);
    JT_REQUIRE(copy_from_user(&s, &b, UBASE + 12 * 4096u + 4000u, 1) == KERR_OK);
    JT_CHECK(byte_ok(UBASE + 12 * 4096u + 4000u, b));
    JT_REQUIRE(copy_from_user(&s, &b, UBASE + 13 * 4096u, 1) == KERR_OK);
    JT_CHECK(byte_ok(UBASE + 13 * 4096u, b));
}

JT_TEST(props_usercopy_write_matches_shadow)
{
    static uint8_t src[5 * 4096u];
    static uint8_t shadow[16u * 4096u];
    static uint8_t now[4096u];
    struct vm_space s = space();
    for (size_t i = 0; i < sizeof shadow; i++) {
        shadow[i] = (uint8_t)((i / 4096u) * 7u + i % 4096u);
    }
    uint64_t r = 0x6789;
    unsigned long ok = 0, refused = 0;
    for (int iter = 0; iter < 20000; iter++) {
        r ^= r >> 12;
        r ^= r << 25;
        r ^= r >> 27;
        uint64_t x = r * 0x2545F4914F6CDD1DULL;
        uint64_t va = UBASE - 4096u + x % (18u * 4096u);
        size_t len = (size_t)((x >> 32) % (5u * 4096u));
        int want_ok = 1;
        for (uint64_t a = va; len != 0 && a < va + len && want_ok; a = (a | 4095u) + 1u) {
            uint64_t i = (a - UBASE) / 4096u;
            if (a < UBASE || i >= 16u || i == 5u || i == 8u || i == 11u || i == 13u) {
                want_ok = 0;
            }
        }
        for (size_t i = 0; i < len; i++) {
            src[i] = (uint8_t)(x >> (i % 7u));
        }
        kerr got = copy_to_user(&s, va, src, len);
        if ((got == KERR_OK) != want_ok || (got != KERR_OK && got != KERR_RANGE)) {
            fprintf(stderr, "  iteration %d: va %" PRIx64 " len %zu: got %d, want %s\n", iter, va,
                    len, got, want_ok ? "OK" : "RANGE");
            JT_CHECK(0);
            return;
        }
        if (got == KERR_OK) {
            ok++;
            if (len != 0) { /* an empty write is fine anywhere, and writes nothing */
                memcpy(&shadow[va - UBASE], src, len);
            }
        } else {
            refused++;
        }
        /* Every 500 writes: all of memory is what the shadow says (a
         * refused write changed nothing, an accepted one only its range). */
        if (iter % 500 == 499) {
            for (unsigned i = 0; i < 16u; i++) {
                if (i == 5u || i == 8u || i == 11u) {
                    continue;
                }
                JT_REQUIRE(copy_from_user(&s, now, UBASE + i * 4096u, 4096u) == KERR_OK);
                if (memcmp(now, &shadow[i * 4096u], 4096u) != 0) {
                    fprintf(stderr, "  iteration %d: page %u differs from the shadow\n", iter, i);
                    JT_CHECK(0);
                    return;
                }
            }
        }
    }
    fprintf(stderr, "  coverage usercopy write ok=%lu refused=%lu\n", ok, refused);
    JT_CHECK(ok > 500 && refused > 500);
}
