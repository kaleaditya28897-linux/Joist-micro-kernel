/* test_map.c - unit tests for frames and mappings (Phase 3 step 3; 0013;
 * invariant I12). The root boots with a 16-page frame in slot F and makes a
 * task T (slot TK) to map it into. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

static struct joist_kernel k;
static joist_cspace root, T;

enum { TK = 0, F = 1, F2 = 2, RO = 3 }; /* root's slots */

static int clean(void)
{
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

static void setup(void)
{
    joist_init(&k);
    if (joist_boot(&k, 64, &root) != JOIST_OK || joist_boot_frame(&k, root, F, 0x100, 16) != JOIST_OK ||
        joist_task_create(&k, root, 20, TK) != JOIST_OK) {
        fprintf(stderr, "  setup failed\n");
        return;
    }
    T = k.obj[k.cs[root].slot[TK].obj - 1u].target - 1u;
}

static uint32_t mappings_in(joist_cspace cs)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < JOIST_MAX_MAPPINGS; i++) {
        n += (uint32_t)(k.map[i].space == cs + 1u);
    }
    return n;
}

JT_TEST(map_boot_frames)
{
    setup();
    struct joist_cap c;
    JT_REQUIRE(joist_cap_read(&k, root, F, &c) == JOIST_OK);
    JT_CHECK(c.type == JOIST_OBJ_FRAME &&
             c.rights == (JOIST_R_CONTROL | JOIST_R_WRITE | JOIST_R_GRANT));
    JT_CHECK(joist_boot_frame(&k, root, F2, 0x10f, 4) == JOIST_E_STATE); /* overlaps the last page */
    JT_CHECK(joist_boot_frame(&k, root, F2, 0x50, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_boot_frame(&k, root, F2, UINT64_MAX, 2) == JOIST_E_RANGE); /* wraps */
    JT_CHECK(joist_boot_frame(&k, root, F, 0x200, 1) == JOIST_E_OCCUPIED);
    JT_CHECK(joist_boot_frame(&k, T, 0, 0x200, 1) == JOIST_E_STATE); /* only the root */
    JT_CHECK(joist_boot_frame(&k, root, F2, 0x110, 4) == JOIST_OK);  /* adjacent is fine */
    JT_CHECK(clean());
}

JT_TEST(map_split)
{
    setup();
    JT_CHECK(joist_frame_split(&k, root, F, 0, F2) == JOIST_E_RANGE);
    JT_CHECK(joist_frame_split(&k, root, F, 16, F2) == JOIST_E_RANGE);
    JT_CHECK(joist_frame_split(&k, root, F, 4, TK) == JOIST_E_OCCUPIED);
    JT_REQUIRE(joist_frame_split(&k, root, F, 4, F2) == JOIST_OK);
    const struct joist_object *a = &k.obj[k.cs[root].slot[F].obj - 1u];
    const struct joist_object *b = &k.obj[k.cs[root].slot[F2].obj - 1u];
    JT_CHECK(a->base == 0x100 && a->npages == 4 && b->base == 0x104 && b->npages == 12);
    JT_CHECK(k.cs[root].slot[F2].rights == k.cs[root].slot[F].rights);
    /* A shared or mapped frame isn't split. */
    JT_REQUIRE(joist_cap_derive(&k, root, F2, RO, JOIST_R_CONTROL) == JOIST_OK);
    JT_CHECK(joist_frame_split(&k, root, F2, 2, 9) == JOIST_E_STATE);
    JT_REQUIRE(joist_map(&k, root, F, TK, 0x400, 0) == JOIST_OK);
    JT_CHECK(joist_frame_split(&k, root, F, 2, 9) == JOIST_E_STATE);
    JT_CHECK(clean());
}

JT_TEST(map_and_unmap)
{
    setup();
    uint32_t q = k.cs[T].quota;
    JT_REQUIRE(joist_map(&k, root, F, TK, 0x400, JOIST_MAP_W) == JOIST_OK);
    JT_CHECK(mappings_in(T) == 1 && k.cs[T].quota == q - 1u); /* the task pays for its tables */
    JT_CHECK(joist_map(&k, root, F, TK, 0x40f, 0) == JOIST_E_OCCUPIED); /* overlaps the last page */
    JT_REQUIRE(joist_map(&k, root, F, TK, 0x410, JOIST_MAP_X) == JOIST_OK); /* aliasing is allowed */
    JT_CHECK(joist_unmap(&k, root, TK, 0x401) == JOIST_E_EMPTY);          /* by its first page */
    JT_REQUIRE(joist_unmap(&k, root, TK, 0x400) == JOIST_OK);
    JT_REQUIRE(joist_unmap(&k, root, TK, 0x410) == JOIST_OK);
    JT_CHECK(mappings_in(T) == 0 && k.cs[T].quota == q);
    JT_CHECK(clean());
}

JT_TEST(map_refusals)
{
    setup();
    JT_CHECK(joist_map(&k, root, F, TK, 0x400, JOIST_MAP_W | JOIST_MAP_X) == JOIST_E_RIGHTS);
    JT_CHECK(joist_map(&k, root, F, TK, 0x400, 4) == JOIST_E_RIGHTS); /* unknown permission */
    JT_REQUIRE(joist_cap_derive(&k, root, F, RO, JOIST_R_CONTROL) == JOIST_OK); /* no WRITE */
    JT_CHECK(joist_map(&k, root, RO, TK, 0x400, JOIST_MAP_W) == JOIST_E_RIGHTS);
    JT_CHECK(joist_map(&k, root, RO, TK, 0x400, 0) == JOIST_OK); /* read-only is fine */
    JT_CHECK(joist_map(&k, root, F, TK, JOIST_USER_PAGES - 8u, 0) == JOIST_E_RANGE);
    JT_CHECK(joist_map(&k, root, F, TK, JOIST_USER_PAGES - 16u, 0) == JOIST_OK); /* just fits */
    JT_CHECK(joist_map(&k, root, F, F2, 0x800, 0) == JOIST_E_EMPTY);
    JT_CHECK(joist_map(&k, root, TK, TK, 0x800, 0) == JOIST_E_TYPE);
    JT_CHECK(clean());
}

JT_TEST(map_needs_the_tasks_quota)
{
    joist_init(&k);
    JT_REQUIRE(joist_boot(&k, 8, &root) == JOIST_OK);
    JT_REQUIRE(joist_boot_frame(&k, root, F, 0x100, 1) == JOIST_OK);
    JT_REQUIRE(joist_boot_frame(&k, root, F2, 0x200, 1) == JOIST_OK);
    JT_REQUIRE(joist_task_create(&k, root, 1, TK) == JOIST_OK); /* one unit to spend */
    T = k.obj[k.cs[root].slot[TK].obj - 1u].target - 1u;
    JT_REQUIRE(joist_map(&k, root, F, TK, 0x400, 0) == JOIST_OK);
    JT_CHECK(joist_map(&k, root, F2, TK, 0x500, 0) == JOIST_E_NOMEM);
    JT_CHECK(mappings_in(T) == 1);
    JT_CHECK(clean());
}

JT_TEST(map_frame_freed_unmaps_everywhere)
{
    setup();
    JT_REQUIRE(joist_task_create(&k, root, 4, 5) == JOIST_OK);
    joist_cspace U = k.obj[k.cs[root].slot[5].obj - 1u].target - 1u;
    JT_REQUIRE(joist_map(&k, root, F, TK, 0x400, 0) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F, 5, 0x800, 0) == JOIST_OK); /* shared */
    uint32_t qt = k.cs[T].quota;
    uint32_t qu = k.cs[U].quota;
    JT_REQUIRE(joist_cap_delete(&k, root, F) == JOIST_OK);
    JT_CHECK(mappings_in(T) == 0 && mappings_in(U) == 0);
    JT_CHECK(k.cs[T].quota == qt + 1u && k.cs[U].quota == qu + 1u);
    JT_CHECK(clean());
}

JT_TEST(map_task_destroy_removes_mappings)
{
    setup();
    JT_REQUIRE(joist_map(&k, root, F, TK, 0x400, 0) == JOIST_OK);
    JT_REQUIRE(joist_task_destroy(&k, root, TK) == JOIST_OK);
    JT_CHECK(mappings_in(T) == 0);
    JT_REQUIRE(joist_cap_delete(&k, root, TK) == JOIST_OK);
    JT_CHECK(k.cs[root].quota == 63 && k.cs[root].charged == 1); /* only the frame is left */
    JT_CHECK(clean());
}

JT_TEST(map_invariants_catch_overlaps)
{
    setup();
    char msg[200];
    JT_REQUIRE(joist_boot_frame(&k, root, F2, 0x200, 4) == JOIST_OK);
    k.obj[k.cs[root].slot[F2].obj - 1u].base = 0x10e; /* now overlapping F */
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    k.obj[k.cs[root].slot[F2].obj - 1u].base = 0x200;
    JT_REQUIRE(joist_map(&k, root, F, TK, 0x400, 0) == JOIST_OK);
    JT_REQUIRE(joist_map(&k, root, F2, TK, 0x500, 0) == JOIST_OK);
    k.map[1].va = 0x402; /* overlapping the first mapping */
    JT_CHECK(joist_check_invariants(&k, msg, sizeof msg) != 0);
    k.map[1].va = 0x500;
    JT_CHECK(clean());
}

JT_TEST(map_into_own_space)
{
    /* The root has no task object (0013): it maps into itself with
     * JOIST_SLOT_SELF, as any task may. */
    setup();
    uint32_t q = k.cs[root].quota;
    JT_REQUIRE(joist_map(&k, root, F, JOIST_SLOT_SELF, 0x600, JOIST_MAP_W) == JOIST_OK);
    JT_CHECK(mappings_in(root) == 1 && k.cs[root].quota == q - 1u);
    JT_CHECK(joist_unmap(&k, root, JOIST_SLOT_SELF, 0x600) == JOIST_OK);
    JT_CHECK(mappings_in(root) == 0 && k.cs[root].quota == q);
    JT_CHECK(joist_unmap(&k, root, JOIST_SLOT_SELF, 0x600) == JOIST_E_EMPTY);
    JT_CHECK(joist_unmap(&k, JOIST_MAX_CSPACES, JOIST_SLOT_SELF, 0x600) == JOIST_E_RANGE);
    JT_CHECK(clean());
}

JT_TEST(map_within_one_page_table)
{
    /* A mapping lies within one aligned JOIST_MAP_SPAN, so it needs at most
     * one table per level and the kernel's table pool can't run out. */
    setup();
    JT_CHECK(joist_map(&k, root, F, TK, 0x600 - 8u, 0) == JOIST_E_RANGE); /* crosses 0x600 */
    JT_CHECK(joist_map(&k, root, F, TK, 0x600 - 16u, 0) == JOIST_OK);    /* ends exactly there */
    JT_REQUIRE(joist_boot_frame(&k, root, F2, 0x1000, JOIST_MAP_SPAN + 1u) == JOIST_OK);
    JT_CHECK(joist_map(&k, root, F2, TK, 0x800, 0) == JOIST_E_RANGE); /* too big for any */
    JT_CHECK(clean());
}
