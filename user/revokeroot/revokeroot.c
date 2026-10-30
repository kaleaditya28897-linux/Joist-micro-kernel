/* SPDX-License-Identifier: GPL-3.0-only */
/* revokeroot.c - decision 0022 from user mode: the batched capability copy
 * (cap_copy_batch) and revoke by kill (obj_kill), through the system-call
 * ABI, as fork and the memory manager will use them. The model's tests
 * cover the rules (model/test/test_kill_copy.c, test_props_kill_copy.c);
 * this checks the calls' registers in and out, and that a killed frame is
 * really gone from the page tables' record (debug_counters). */
#include "../lib/loader.h"

#define KILL_VA 0x30000000ull /* the root's own mapping of the frame it kills */

static struct j_counters cnt;

static uint64_t mappings(void)
{
    must(j_debug_counters(&cnt), "debug_counters");
    return cnt.mappings;
}

static void expect(uint64_t got, uint64_t want, const char *what)
{
    if (got != want) {
        j_puts("revoke: ");
        j_puts(what);
        j_puts(": got ");
        j_putu(got);
        j_puts(", wanted ");
        j_putu(want);
        j_puts("\n");
        j_exit(1);
    }
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free);

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free)
{
    (void)nmods;
    (void)ld_load; /* (no modules to load here) */
    ld_init(pool_first, pool_n, mod_first, first_free);
    uint64_t task = slot_new();
    must(j_task_create(8, task), "task_create");
    ld_tables(task); /* pages for its page tables (0021) */
    uint64_t frame = page_alloc();
    must(j_debug_counters(&cnt), "debug_counters");
    uint64_t objects = cnt.objects;

    /* Six slots in a row; the copies land at the same numbers in the task. */
    uint64_t s = slot_new();
    for (int i = 1; i < 6; i++) {
        if (slot_new() != s + (uint64_t)i) {
            die("six slots in a row");
        }
    }
    must(j_obj_create(J_ENDPOINT, s), "obj_create");                 /* RECV: skipped */
    must(j_cap_mint(s, s + 1, J_R_SEND | J_R_GRANT, 5), "cap_mint"); /* copied */
    must(j_cap_derive(s, s + 2, J_R_SEND), "cap_derive");            /* no GRANT: skipped */
    must(j_cap_derive(frame, s + 3, J_R_ALL), "cap_derive (frame)"); /* copied */
    must(j_obj_create(J_NOTIFICATION, s + 5), "obj_create");         /* copied; s + 4 empty */

    uint64_t done = 99;
    uint64_t skipped = 99;
    expect(j_cap_copy_batch(task, s, 6, J_R_ALL & ~J_R_KILL, 0, &done, &skipped), J_OK, "batch");
    expect(done, 6, "batch: done");
    expect(skipped, 0x5, "batch: skipped");
    /* Again: stops at once on s + 1, now taken in the task. */
    expect(j_cap_copy_batch(task, s, 6, J_R_ALL, 0, &done, &skipped), J_E_OCCUPIED, "batch again");
    expect(done, 1, "batch again: done");
    expect(skipped, 0x1, "batch again: skipped");
    expect(j_cap_copy_batch(task, s, J_COPY_BATCH + 1u, J_R_ALL, 0, &done, &skipped), J_E_RANGE,
           "batch of 33");
    expect(j_cap_copy_batch(task, s, 1, J_R_ALL, 2, &done, &skipped), J_E_RANGE, "unknown flag");
    expect(j_cap_copy_batch(task, s, 1, J_R_ALL, J_COPY_RECV, &done, &skipped), J_OK,
           "batch with receive rights");
    expect(done * 16u + skipped, 16, "batch with receive rights: done, skipped");
    j_puts("revoke: batch ok\n");

    /* The frame: mapped writable here, read-only in the task, then killed. */
    uint64_t m0 = mappings();
    must(j_map(frame, J_ROOT_SELF, KILL_VA / J_PAGE, J_MAP_W), "map (here)");
    must(j_map(s + 3, task, CHILD_LO / J_PAGE, 0), "map (the task)");
    *(volatile uint8_t *)(uintptr_t)KILL_VA = 0x5a;
    expect(mappings(), m0 + 2u, "mappings");
    expect(j_obj_kill(s), J_E_TYPE, "kill an endpoint");
    must(j_cap_derive(frame, s + 4, J_R_ALL & ~J_R_KILL), "cap_derive (no kill right)");
    expect(j_obj_kill(s + 4), J_E_RIGHTS, "kill without the kill right");
    expect(j_obj_kill(frame), J_OK, "kill");
    int more = 1;
    while (more) {
        must(j_continue(0, &more), "continue"); /* the sweep, if the kernel was preempted */
    }
    expect(mappings(), m0, "mappings after the kill");
    expect(j_map(frame, J_ROOT_SELF, KILL_VA / J_PAGE, 0), J_E_GONE, "map a killed frame");
    expect(j_map(s + 3, task, CHILD_LO / J_PAGE, 0), J_E_GONE, "map a killed frame's copy");
    expect(j_frame_split(s + 4, 1, s + 2), J_E_GONE, "split a killed frame");
    expect(j_obj_kill(frame), J_E_GONE, "kill twice");
    j_puts("revoke: kill ok\n");

    /* The task destroyed: a batch into it is E_DEAD; and everything goes. */
    must(j_task_destroy(task), "task_destroy");
    expect(j_cap_copy_batch(task, s, 6, J_R_ALL, 0, &done, &skipped), J_E_DEAD, "batch, dead task");
    expect(done + skipped, 0, "batch, dead task: done, skipped");
    for (int i = 0; i < 6; i++) {
        (void)j_cap_delete(s + (uint64_t)i);
    }
    must(j_cap_delete(frame), "cap_delete (frame)");
    must(j_cap_delete(task), "cap_delete (task)");
    must(j_debug_counters(&cnt), "debug_counters");
    expect(cnt.objects, objects - 2u, "objects left"); /* the killed frame and the task */
    j_puts("revoke: dead task ok\n");
    j_puts("revoke: all ok\n");
    j_exit(0);
}
