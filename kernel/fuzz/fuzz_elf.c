/* fuzz_elf.c - libFuzzer on the root task's ELF validator (Phase 2 step 6;
 * §8.1). Any input: no out-of-bounds read (ASan), no undefined behaviour
 * (UBSan), and every accepted image keeps elf.h's promises. */
#include "elf.h"
#include "elf_promises.h"

#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char why[128];
    struct elf_image im;
    if (elf_validate(data, size, 0x400000u, 0x800000000000ull, &im) == KERR_OK &&
        !elf_promises_hold(&im, size, 0x400000u, 0x800000000000ull, why, sizeof why)) {
        fprintf(stderr, "fuzz_elf: accepted an image where %s\n", why);
        abort();
    }
    return 0;
}
