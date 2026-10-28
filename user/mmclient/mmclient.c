/* mmclient - a task the memory manager serves (Phase 4 step 6, part D;
 * decision 0023), run by mmroot. Its role is in its work page (work.h:
 * MMC_*); it reaches mm through its capability in MM_SLOT, and mm answers
 * its page faults. */
#include "../lib/joist.h"
#include "mm_proto.h"
#include "mmlib.h"
#include "../lib/work.h"

void _start(void);

#define PAGES 64u
#define WORDS (J_PAGE / 8u)

static uint64_t req(uint64_t r0, uint64_t a, uint64_t b, uint64_t c)
{
    struct j_msg m = {.r = {r0, a, b, c}};
    uint64_t e = j_call(MM_SLOT, &m, J_FOREVER);
    return e != J_OK ? 0xffu : m.r[0];
}

static volatile uint64_t *page(uint64_t i)
{
    return (volatile uint64_t *)(uintptr_t)(MMC_VA + i * J_PAGE);
}

/* The value it writes at word `w` of page `i`. */
static uint64_t val(uint64_t i, uint64_t w)
{
    return (i + 1u) << 32 | (w * 7u + 1u);
}

static uint64_t basic(void)
{
    uint64_t va = MMC_VA / J_PAGE;
    if (req(MM_REQ_MAP_ANON, va, PAGES, MM_W) != MM_OK) {
        return 1;
    }
    if (req(MM_REQ_MAP_ANON, va + 10u, 4, MM_W) == MM_OK) {
        return 2; /* overlaps the first */
    }
    if (req(MM_REQ_MAP_ANON, va + PAGES, 4, MM_W | MM_X) == MM_OK) {
        return 3; /* W^X */
    }
    /* Demand-zero: a read of fresh memory finds zeroes (and maps the pages
     * around it), a write maps one page. */
    if (page(0)[0] != 0 || page(5)[WORDS - 1u] != 0) {
        return 4;
    }
    for (uint64_t i = 0; i < PAGES; i++) {
        page(i)[0] = val(i, 0);
        page(i)[WORDS - 1u] = val(i, WORDS - 1u);
    }
    for (uint64_t i = 0; i < PAGES; i++) {
        if (page(i)[0] != val(i, 0) || page(i)[WORDS - 1u] != val(i, WORDS - 1u) ||
            page(i)[1] != 0) {
            return 5;
        }
    }
    /* Read-only: still readable. */
    if (req(MM_REQ_PROTECT, va, PAGES / 2u, 0) != MM_OK) {
        return 6;
    }
    for (uint64_t i = 0; i < PAGES / 2u; i++) {
        if (page(i)[0] != val(i, 0)) {
            return 7;
        }
    }
    /* Unmapped, then asked for again: fresh zeroes. */
    if (req(MM_REQ_UNMAP, va + PAGES / 2u, PAGES / 2u, 0) != MM_OK) {
        return 8;
    }
    if (req(MM_REQ_MAP_ANON, va + PAGES / 2u, PAGES / 2u, MM_W) != MM_OK) {
        return 9;
    }
    for (uint64_t i = PAGES / 2u; i < PAGES; i++) {
        if (page(i)[0] != 0) {
            return 10;
        }
        page(i)[0] = val(i, 0);
    }
    /* Not this task's to ask, or not well formed. */
    if (req(MM_REQ_ADOPT, MM_S_ROOT, 0, 0) != MM_E_STATE) {
        return 11;
    }
    if (req(MM_REQ_PROTECT, va, 1, 7) != MM_E_RANGE) {
        return 12;
    }
    if (req(MM_REQ_UNMAP, va + PAGES, 1, 0) == 0xffu) {
        return 13;
    }
    if (req(MM_REQ_CLONE, 0, 0, 0) != MM_E_STATE) {
        return 14; /* part D2 */
    }
    /* What was written is still there. */
    for (uint64_t i = 0; i < PAGES; i++) {
        if (page(i)[0] != val(i, 0)) {
            return 15;
        }
    }
    return 0;
}

void _start(void)
{
    struct work *w = (struct work *)(uintptr_t)WORK_VA;
    w->count = 1;
    switch (w->role) {
    case MMC_BASIC:
        w->arg = basic();
        break;
    case MMC_RO_WRITE:
        if (req(MM_REQ_MAP_ANON, MMC_VA / J_PAGE, 4, MM_W) != MM_OK) {
            w->arg = 1;
            break;
        }
        page(0)[0] = 1;
        if (req(MM_REQ_PROTECT, MMC_VA / J_PAGE, 4, 0) != MM_OK) {
            w->arg = 2;
            break;
        }
        w->aux = 1;
        page(0)[0] = 2; /* mm ends the thread here */
        w->arg = 3;
        break;
    case MMC_OUTSIDE:
        w->aux = 1;
        w->arg = page(PAGES * 4u)[0]; /* no region: mm ends the thread */
        w->arg = 3;
        break;
    default:
        w->arg = 99;
        break;
    }
    w->count = 2;
    j_exit(0);
}
