/* test_elf.c - the root task's ELF validator (Phase 2 step 6): unit tests,
 * and a property test that corrupts valid images and checks every image the
 * validator accepts against its promises (elf.h). */
#include "elf.h"
#include "elf_promises.h"
#include "harness.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define ULO 0x400000u /* user range used throughout */
#define UHI 0x800000000000ull

static uint8_t img[8192];

static void w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void w32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void w64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* Program header i of the image under construction. */
static uint8_t *ph(int i)
{
    return img + 64 + 56 * i;
}

static void set_ph(int i, uint32_t type, uint32_t flags, uint64_t off, uint64_t vaddr,
                   uint64_t filesz, uint64_t memsz)
{
    w32(ph(i), type);
    w32(ph(i) + 4, flags);
    w64(ph(i) + 8, off);
    w64(ph(i) + 16, vaddr);
    w64(ph(i) + 24, vaddr); /* p_paddr: unused */
    w64(ph(i) + 32, filesz);
    w64(ph(i) + 40, memsz);
    w64(ph(i) + 48, 4096);
}

/* A valid two-segment executable: text (R+X) at 0x401000 from file offset
 * 0x1000, data (R+W) at 0x402000 from 0x1800, with .bss. */
static size_t build(void)
{
    memset(img, 0, sizeof img);
    img[0] = 0x7f;
    img[1] = 'E';
    img[2] = 'L';
    img[3] = 'F';
    img[4] = 2;        /* ELFCLASS64 */
    img[5] = 1;        /* little-endian */
    img[6] = 1;        /* EV_CURRENT */
    w16(img + 16, 2);  /* ET_EXEC */
    w16(img + 18, 62); /* EM_X86_64 */
    w32(img + 20, 1);
    w64(img + 24, 0x401010); /* entry */
    w64(img + 32, 64);       /* phoff */
    w16(img + 52, 64);
    w16(img + 54, 56);
    w16(img + 56, 3);
    set_ph(0, 1, 5, 0x1000, 0x401000, 0x200, 0x200);
    set_ph(1, 1, 6, 0x1800, 0x402800, 0x100, 0x2000);
    set_ph(2, 0x6474e551u, 6, 0, 0, 0, 0); /* PT_GNU_STACK: ignored */
    return 0x1900;
}

static kerr check(size_t size)
{
    struct elf_image im;
    return elf_validate(img, size, ULO, UHI, &im);
}

JT_TEST(elf_accepts_a_valid_executable)
{
    size_t n = build();
    struct elf_image im;
    JT_REQUIRE(elf_validate(img, n, ULO, UHI, &im) == KERR_OK);
    JT_CHECK(im.entry == 0x401010 && im.nsegments == 2);
    JT_CHECK(im.seg[0].vaddr == 0x401000 && im.seg[0].flags == (ELF_SEG_R | ELF_SEG_X));
    JT_CHECK(im.seg[1].memsz == 0x2000 && im.seg[1].filesz == 0x100 &&
             im.seg[1].flags == (ELF_SEG_R | ELF_SEG_W));
}

JT_TEST(elf_header_checks)
{
    size_t n = build();
    JT_CHECK(check(63) == KERR_INVAL); /* shorter than a header */
    img[0] = 0x7e;
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    img[4] = 1; /* 32-bit */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    img[5] = 2; /* big-endian */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w16(img + 16, 3); /* ET_DYN */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w16(img + 18, 183); /* aarch64 */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w16(img + 54, 64); /* wrong phentsize */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w16(img + 56, 0);
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w16(img + 56, 17); /* more than ELF_MAX_PHDRS */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w64(img + 32, UINT64_MAX - 10); /* phoff wraps */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    JT_CHECK(check(64 + 56 * 3 - 1) == KERR_INVAL); /* headers cut short */
    struct elf_image im;
    JT_CHECK(elf_validate(img, n, UHI, ULO, &im) == KERR_INVAL); /* empty user range */
    JT_CHECK(elf_validate(NULL, n, ULO, UHI, &im) == KERR_INVAL);
}

/* Program headers running past the end of the file, with nothing else
 * wrong: the segments carry no file bytes, so only the header bound can
 * refuse it. The image is copied into an exact-sized heap buffer, so an
 * over-read is also an ASan error, not a read of the test's spare space. */
JT_TEST(elf_phdrs_past_end_refused)
{
    build();
    set_ph(0, 1, 5, 0, 0x401000, 0, 0x200); /* text: no file bytes */
    set_ph(1, 1, 6, 0, 0x403000, 0, 0x100); /* data: no file bytes */
    size_t n = 64 + 56 * 2 + 10;            /* the third header is cut off */
    uint8_t *exact = malloc(n);
    JT_REQUIRE(exact != NULL);
    memcpy(exact, img, n);
    struct elf_image im;
    JT_CHECK(elf_validate(exact, n, ULO, UHI, &im) == KERR_INVAL);
    free(exact);
    /* The same image with all three headers present is valid. */
    JT_CHECK(check(64 + 56 * 3) == KERR_OK);
}

JT_TEST(elf_segment_checks)
{
    size_t n = build();
    w64(ph(0) + 32, 0x300); /* filesz > memsz */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w64(ph(1) + 8, 0x1890); /* file bytes past the end */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w64(ph(0) + 8, UINT64_MAX - 0xfff); /* offset wraps */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w64(ph(0) + 16, 0x401100); /* vaddr and offset not congruent mod 4 KiB */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w64(ph(0) + 16, 0x1000); /* below the user range */
    w64(img + 24, 0x1010);
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w64(ph(1) + 40, UHI); /* runs past the user range */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w64(ph(1) + 16, UINT64_MAX - 0x7ff); /* vaddr + memsz wraps */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w64(ph(1) + 16, 0x401800); /* shares text's page */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    set_ph(0, 1, 6, 0x1800, 0x402800, 0x100, 0x2000); /* unsorted: data first */
    set_ph(1, 1, 5, 0x1000, 0x401000, 0x200, 0x200);
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w32(ph(1) + 4, 7); /* R+W+X */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w64(ph(0) + 40, 0); /* empty segment */
    w64(ph(0) + 32, 0);
    JT_CHECK(check(n) == KERR_INVAL);
}

JT_TEST(elf_entry_and_types)
{
    size_t n = build();
    w64(img + 24, 0x402900); /* in the data segment, not executable */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w64(img + 24, 0x401200); /* just past text */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w32(ph(2), 3); /* PT_INTERP */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w32(ph(2), 2); /* PT_DYNAMIC */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w32(ph(2), 7); /* PT_TLS */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w32(ph(0), 4); /* text becomes a note: no executable segment left */
    JT_CHECK(check(n) == KERR_INVAL);
    n = build();
    w32(ph(2), 4); /* a note is ignored */
    JT_CHECK(check(n) == KERR_OK);
}

/* ---- property test ---- */

static uint64_t rng_state;

static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1DULL;
}

JT_TEST(props_elf_corruption)
{
    char why[128];
    unsigned long accepted = 0;
    unsigned long refused = 0;
    rng_state = 0x5eed;
    for (int iter = 0; iter < 200000; iter++) {
        size_t n = build();
        /* Corrupt: flip bits, overwrite header fields, or truncate. */
        int edits = 1 + (int)(rng() % 4);
        for (int e = 0; e < edits; e++) {
            uint64_t r = rng();
            switch (r % 4) {
            case 0: /* a bit anywhere in the headers */
                img[(r >> 8) % (64 + 56 * 3)] ^= (uint8_t)(1u << ((r >> 32) % 8));
                break;
            case 1: /* a program-header word set to something nasty */
                w64(ph((int)((r >> 8) % 3)) + 8 * (1 + (r >> 16) % 5),
                    (r >> 40) % 2 ? (rng() & 0xffffffu) : rng());
                break;
            case 2: /* phoff / phnum / entry */
                if ((r >> 8) % 3 == 0) {
                    w64(img + 32, rng() % 300);
                } else if ((r >> 8) % 3 == 1) {
                    w16(img + 56, (uint16_t)(rng() % 20));
                } else {
                    w64(img + 24, 0x400000u + rng() % 0x4000);
                }
                break;
            default: /* truncate */
                n = (size_t)(rng() % (n + 1));
                break;
            }
        }
        struct elf_image im;
        if (elf_validate(img, n, ULO, UHI, &im) == KERR_OK) {
            accepted++;
            if (!elf_promises_hold(&im, n, ULO, UHI, why, sizeof why)) {
                fprintf(stderr, "  iteration %d accepted an image that %s\n", iter, why);
                JT_CHECK(0);
                return;
            }
        } else {
            refused++;
        }
    }
    fprintf(stderr, "  coverage accepted=%lu refused=%lu\n", accepted, refused);
    JT_CHECK(accepted > 1000 && refused > 1000); /* both sides exercised */
}
