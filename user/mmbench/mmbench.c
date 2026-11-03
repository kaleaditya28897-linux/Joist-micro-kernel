/* SPDX-License-Identifier: GPL-3.0-only */
/* mmbench - the memory manager's costs, measured (Phase 4's exit;
 * docs/phase4-data/step7-exit). Module 0 is mm, module 1 mmclient, run as
 * MMC_BENCH: it times, with the TSC, demand-zero faults (a write, and a
 * read that maps MM_AROUND pages), a write that copies a page another task
 * shares (COW), one that only makes the last reference writable, a request
 * (MM_REQ_ID), protect over a table's span, SHARE of 64 pages into a child
 * (which this root starts, as mmroot does), and clones of 1, 16 and 256 MiB
 * as far as MMB_CLONE_PAGES allows, and a churn (map, write every page,
 * unmap; 1, 4, 16 and 64 pages) by the client's own region calls against
 * the same by requests and faults, each with its page table emptied by
 * every unmap and with a page kept in it (Phase 5 step 3). Here: nosenders_next finding nothing,
 * which mm calls at the start of every message (0023's amendment). Each is
 * printed as a median and the fastest, in cycles.
 *
 * The default build (the user programs' mm sizes) clones up to 1 MiB; the
 * larger clones need a build with larger tables and pool (README.md there). */
#define TEST_NAME "mmbench"
#include "../lib/testroot.h"
#ifndef MMB_POOL
#define MMB_POOL 512u
#endif
#ifndef MMB_CLONE_PAGES
#define MMB_CLONE_PAGES 256u
#endif
#define HOST_POOL_PAGES MMB_POOL
#define HOST_TABLE_RUNS 4u
#ifdef MMB_TABLES
#define HOST_MM_TABLES MMB_TABLES
#endif
#include "../lib/mmhost.h"

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus);

#define N 240u
static uint64_t smp[N];

static uint64_t median_of(uint64_t *v, unsigned n, uint64_t *fastest)
{
    for (unsigned i = 1; i < n; i++) { /* insertion sort: n is small */
        uint64_t x = v[i];
        unsigned j = i;
        for (; j > 0 && v[j - 1u] > x; j--) {
            v[j] = v[j - 1u];
        }
        v[j] = x;
    }
    *fastest = n != 0 ? v[0] : 0;
    return n != 0 ? v[n / 2u] : 0;
}

static void put(const char *what, uint64_t median, uint64_t fastest)
{
    j_puts("mmbench: ");
    j_puts(what);
    j_puts(": median ");
    j_putu(median);
    j_puts(", fastest ");
    j_putu(fastest);
    j_puts(" cycles\n");
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free, uint64_t ncpus)
{
    ld_init(pool_first, pool_n, mod_first, first_free);
    if (nmods < 2) {
        die("needs two modules: mm, mmclient");
    }
    test_init();
    host_init();
    start_mm();
    j_puts("mmbench: cpus ");
    j_putu(ncpus);
    j_puts(", pool ");
    j_putu(MMB_POOL);
    j_puts(" pages, clones up to ");
    j_putu(MMB_CLONE_PAGES);
    j_puts(" pages\n");

    /* nosenders_next finding nothing, on mm's endpoint (the root holds it
     * with RECV too): what mm's collection costs a message. */
    for (unsigned i = 0; i < N; i++) {
        uint64_t b;
        uint64_t t0 = work_tsc();
        uint64_t e = j_nosenders_next(mm_ep, &b);
        smp[i] = work_tsc() - t0;
        check(e == J_OK && b == 0, "nosenders_next found a badge");
    }
    uint64_t fast;
    uint64_t med = median_of(smp, N, &fast);
    put("nosenders_next, nothing queued", med, fast);

    struct worker k;
    k.task = slot_new();
    k.thread = slot_new();
    k.sc = slot_new();
    uint64_t entry = ld_load(1, k.task);
#ifdef MMB_TABLES
    must(j_space_donate(k.task, frame_take(MMB_TABLES)), "space_donate (client)");
#endif
    uint64_t f = page_alloc();
    must(j_map(f, J_ROOT_SELF, ROOT_WORK_VA / J_PAGE, J_MAP_W), "map (work page, here)");
    k.w = (struct work *)(uintptr_t)ROOT_WORK_VA;
    k.w->role = MMC_BENCH;
    k.w->arg = MMB_CLONE_PAGES;
    k.w->count = 0;
    k.w->aux = 0;
    must(j_map(f, k.task, WORK_VA / J_PAGE, J_MAP_W), "map (work page, client)");
    must(j_cap_give(mm_task, k.task, MM_S_ROOT, J_R_ALL), "cap_give (client, to mm)");
    struct j_msg r = ask(MM_REQ_ADOPT, MM_S_ROOT);
    check(r.r[0] == MM_OK, "mm's ADOPT failed");
    unsigned ri = rec_new();
    must(j_cap_derive(k.task, recs[ri].task, J_R_ALL), "cap_derive (client)");
    give_root_cap(ri); /* for SHARE's child */
    must(j_thread_spawn(k.task, k.thread, entry, CHILD_LO), "thread_spawn (client)");
    worker_run(&k, 100, 10 * J_MS, 10 * J_MS);
    for (unsigned i = 0; i < 600000u && k.w->count < 2; i++) {
        unsigned kid;
        (void)host_serve(J_MS, &kid);
    }
    if (k.w->count != 2 || k.w->aux != 0) {
        j_puts("mmbench: the client failed at step ");
        j_putu(k.w->aux);
        j_puts("\n");
        check(0, "the benchmark didn't finish");
    }
    static const char *const names[] = {
        "a demand-zero write fault (one page)",
        "a demand-zero read fault (16 pages)",
        "a write fault that copies (COW, another reference)",
        "a write fault that upgrades (COW, the last reference)",
        "a request (MM_REQ_ID), round trip",
        "protect, 512 pages, to read-only",
        "protect, 512 pages, to writable",
        "SHARE, 64 pages none there yet, round trip",
    };
    for (unsigned i = 0; i < MMC_B_CLONE; i++) {
        put(names[i], MMC_W(k.w, MMC_B + 2u * i), MMC_W(k.w, MMC_B + 2u * i + 1u));
    }
    static const uint64_t sizes[] = {MMC_BENCH_SIZES};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0] && sizes[i] <= MMB_CLONE_PAGES; i++) {
        static const char *const clones[] = {"CLONE of 1 MiB, every page mapped",
                                             "CLONE of 16 MiB, every page mapped",
                                             "CLONE of 256 MiB, every page mapped"};
        put(clones[i], MMC_W(k.w, MMC_B + 2u * (MMC_B_CLONE + i)),
            MMC_W(k.w, MMC_B + 2u * (MMC_B_CLONE + i) + 1u));
    }
    static const char *const churn[] = {
        "churn of 1 page, region_map, write, region_unmap",
        "churn of 4 pages, region_map, writes, region_unmap",
        "churn of 16 pages, region_map, writes, region_unmap",
        "churn of 64 pages, region_map, writes, region_unmap",
        "churn of 1 page, MAP_ANON, write fault, UNMAP",
        "churn of 4 pages, MAP_ANON, write faults, UNMAP",
        "churn of 16 pages, MAP_ANON, write faults, UNMAP",
        "churn of 64 pages, MAP_ANON, write faults, UNMAP",
        "churn of 1 page, table kept, region_map, write, region_unmap",
        "churn of 4 pages, table kept, region_map, writes, region_unmap",
        "churn of 16 pages, table kept, region_map, writes, region_unmap",
        "churn of 64 pages, table kept, region_map, writes, region_unmap",
        "churn of 1 page, table kept, MAP_ANON, write fault, UNMAP",
        "churn of 4 pages, table kept, MAP_ANON, write faults, UNMAP",
        "churn of 16 pages, table kept, MAP_ANON, write faults, UNMAP",
        "churn of 64 pages, table kept, MAP_ANON, write faults, UNMAP",
    };
    for (unsigned i = 0; i < sizeof churn / sizeof churn[0]; i++) {
        put(churn[i], MMC_W(k.w, MMC_B + 2u * (MMC_B_CHURN + i)),
            MMC_W(k.w, MMC_B + 2u * (MMC_B_CHURN + i) + 1u));
    }
    j_puts("mmbench: done\n");
    j_exit(0);
}
