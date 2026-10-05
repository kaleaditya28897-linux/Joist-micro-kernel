/* test_usercopy.c - copy_from_user (Phase 2 step 6) over simulated physical
 * memory: unit tests and a property test against a shadow of the mappings. */
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
 * without VM_USER (kernel-only), page 8 not at all. */
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
        unsigned perms = (i == 5u || i == 11u) ? VM_W : (VM_W | VM_USER);
        if (vm_map(&s, UBASE + i * 4096u, pa, 4096u, VM_PAGE, perms) != KERR_OK) {
            abort();
        }
    }
    /* Something in the kernel half, mapped user-accessible on purpose: a
     * pointer there must still be refused by the half check. */
    uint64_t kpa;
    if (get_frame(NULL, &kpa) != KERR_OK ||
        vm_map(&s, 0xffff800000000000ull, kpa, 4096u, VM_PAGE, VM_USER) != KERR_OK) {
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
