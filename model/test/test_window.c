/* SPDX-License-Identifier: GPL-3.0-only */
/* test_window.c - bulk data through the caller's windows (decision 0032;
 * Phase 5 step 9): a caller in task T sets a read and a write window; a
 * server in the root, holding the call's reply, copies from the one and
 * into the other with reply_read and reply_write. Memory is simulated, so
 * every byte can be checked: bytes land only inside the window, only in
 * pages that are present, writable and in a live region of their space,
 * only while the caller waits; a refused call changes nothing.
 *
 * The world: simulated pages from MEM_BASE. T has a one-table region at LO
 * with an 8-page extent: pages LO..LO+5 mapped writable, LO+6 read-only,
 * LO+7 not mapped; and one plain writable page (not in a region) at PL. The
 * root has the same shape at LO in its own space. */
#include "harness.h"
#include "internal.h"
#include "joist/core.h"

#include <stdlib.h>
#include <string.h>

static struct joist_kernel k;
static joist_cspace root, T;
static joist_thread c, s;

enum { TK = 0, SELF = 1, EP = 2, RT = 3, RR = 4, F_DT = 5, F_DR = 6, F_ET = 7, F_ER = 8, F_PT = 9,
       F_PR = 10, ROBJ = 11 };
enum { T_EP = 0 }; /* the endpoint, in T's space */

#define MEM_BASE 0x4000u
#define MEM_PAGES 96u
static uint8_t mem[MEM_PAGES][JOIST_PAGE_SIZE];

#define LO ((uint64_t)8 * JOIST_PT_ENTRIES)            /* the regions' first page */
#define PL ((uint64_t)12 * JOIST_PT_ENTRIES + 5u)      /* a plain page, in no region */
#define VA(p) ((uint64_t)(p) * JOIST_PAGE_SIZE)

/* Any page asked for outside the simulated memory is counted: the core
 * reaches only pages a present entry names, and in this world those are
 * all simulated. (A copy that followed an absent entry would ask for the
 * page of frame 0's base: in the kernel, any physical memory.) */
static unsigned outside;

static void *page(void *ctx, uint64_t pfn)
{
    (void)ctx;
    if (pfn >= MEM_BASE && pfn < MEM_BASE + MEM_PAGES) {
        return mem[pfn - MEM_BASE];
    }
    outside++;
    return NULL;
}

static int clean(void)
{
    if (outside != 0) {
        fprintf(stderr, "  %u page(s) asked for outside the simulated memory\n", outside);
        return 0;
    }
    char msg[200];
    unsigned n = joist_check_invariants(&k, msg, sizeof msg);
    if (n != 0) {
        fprintf(stderr, "  invariants: %u violation(s), first: %s\n", n, msg);
    }
    return n == 0;
}

/* The byte at user address va of space sp, through the core's own entry
 * (NULL: not present). */
static uint8_t *byte_at(joist_cspace sp, uint64_t va)
{
    uint64_t e = joist__pte(&k, sp, va / JOIST_PAGE_SIZE);
    if ((e & JOIST_PTE_P) == 0) {
        return NULL;
    }
    uint8_t *p = page(NULL, k.obj[JOIST_PTE_FRAME(e)].base + JOIST_PTE_OFF(e));
    return p != NULL ? p + va % JOIST_PAGE_SIZE : NULL;
}

/* A space's region over [LO, LO + 1 table) with frame `ext` (8 pages) as its
 * extent: 6 pages writable, 1 read-only, 1 not mapped; and frame `plain`'s
 * page at PL, writable. Through the task capability in task_slot. */
static int shape(joist_slot task_slot, joist_slot reg, joist_slot ext, joist_slot plain)
{
    uint32_t x;
    return joist_region_create(&k, root, task_slot, LO, 1, JOIST_MAP_W, reg) == JOIST_OK &&
           joist_region_add(&k, root, reg, ext, 0, 8, &x) == JOIST_OK &&
           joist_region_map(&k, root, reg, x, 0, 6, LO, JOIST_MAP_W) == JOIST_OK &&
           joist_region_map(&k, root, reg, x, 6, 1, LO + 6u, 0) == JOIST_OK &&
           joist_map(&k, root, plain, 0, 1, task_slot, PL, JOIST_MAP_W) == JOIST_OK;
}

/* The world, the server waiting on the endpoint, memory patterned (T's
 * bytes from 0x10, the root's from 0x80, varying). */
static void world(void)
{
    joist_init(&k);
    k.hooks.page = page;
    outside = 0;
    for (uint32_t p = 0; p < MEM_PAGES; p++) {
        for (uint32_t i = 0; i < JOIST_PAGE_SIZE; i++) {
            mem[p][i] = (uint8_t)(p * 7u + i * 13u + (p < 48u ? 0x10u : 0x80u));
        }
    }
    if (joist_boot(&k, 200, &root) != JOIST_OK || joist_boot_self(&k, root, SELF) != JOIST_OK ||
        joist_boot_frame(&k, root, F_DT, MEM_BASE, 16) != JOIST_OK ||
        joist_boot_frame(&k, root, F_ET, MEM_BASE + 16u, 8) != JOIST_OK ||
        joist_boot_frame(&k, root, F_PT, MEM_BASE + 24u, 1) != JOIST_OK ||
        joist_boot_frame(&k, root, F_DR, MEM_BASE + 48u, 16) != JOIST_OK ||
        joist_boot_frame(&k, root, F_ER, MEM_BASE + 64u, 8) != JOIST_OK ||
        joist_boot_frame(&k, root, F_PR, MEM_BASE + 72u, 1) != JOIST_OK ||
        joist_task_create(&k, root, 40, TK) != JOIST_OK ||
        joist_space_donate(&k, root, TK, F_DT) != JOIST_OK ||
        joist_space_donate(&k, root, SELF, F_DR) != JOIST_OK || !shape(TK, RT, F_ET, F_PT) ||
        !shape(SELF, RR, F_ER, F_PR) || joist_obj_create(&k, root, JOIST_OBJ_ENDPOINT, EP) != JOIST_OK ||
        joist_obj_create(&k, root, JOIST_OBJ_REPLY, ROBJ) != JOIST_OK ||
        joist_cap_give(&k, root, TK, EP, T_EP, JOIST_R_SEND) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's world could not be set up as written", 0);
        return;
    }
    T = k.obj[k.cs[root].slot[TK].obj - 1u].target - 1u;
    if (joist_thread_create(&k, T, &c) != JOIST_OK || joist_thread_create(&k, root, &s) != JOIST_OK) {
        jt_fail(__FILE__, __LINE__, "the test's threads could not be made", 0);
    }
}

/* c calls the server, which receives it: s holds c's reply. */
static int serving(void)
{
    struct joist_msg m = {.r = {1}};
    return joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED &&
           joist_call(&k, c, T_EP, &m, JOIST_FOREVER) == JOIST_BLOCKED && k.th[s].reply_to == c + 1u;
}

static uint8_t snap[MEM_PAGES][JOIST_PAGE_SIZE];

JT_TEST(window_set_and_its_refusals)
{
    world();
    uint64_t lo = VA(LO);
    JT_CHECK(joist_thread_window(&k, c, 2, lo, 10) == JOIST_E_RANGE); /* no such kind */
    JT_CHECK(joist_thread_window(&k, c, JOIST_WIN_WRITE, VA(PL), 10) == JOIST_E_RANGE); /* no region */
    JT_CHECK(joist_thread_window(&k, c, JOIST_WIN_WRITE, lo + VA(JOIST_PT_ENTRIES) - 10u, 20) ==
             JOIST_E_RANGE); /* runs out of the region */
    JT_CHECK(joist_thread_window(&k, c, JOIST_WIN_READ, VA(JOIST_USER_PAGES) - 4u, 8) == JOIST_E_RANGE);
    JT_CHECK(joist_thread_window(&k, c, JOIST_WIN_READ, UINT64_MAX - 3u, 8) == JOIST_E_RANGE); /* wraps */
    JT_CHECK(k.th[c].win[0].len == 0 && k.th[c].win[1].len == 0); /* refused: nothing set */
    /* A whole region's table as a write window; any memory as a read one. */
    JT_CHECK(joist_thread_window(&k, c, JOIST_WIN_WRITE, lo, VA(JOIST_PT_ENTRIES)) == JOIST_OK);
    JT_CHECK(joist_thread_window(&k, c, JOIST_WIN_READ, VA(PL) + 7u, 100) == JOIST_OK);
    JT_CHECK(k.th[c].win[1].va == lo && k.th[c].win[0].va == VA(PL) + 7u && k.th[c].win[0].len == 100);
    JT_CHECK(joist_thread_window(&k, c, JOIST_WIN_READ, 1234, 0) == JOIST_OK && k.th[c].win[0].len == 0 &&
             k.th[c].win[0].va == 0); /* cleared */
    /* Only a running thread sets its own. */
    JT_REQUIRE(serving());
    JT_CHECK(joist_thread_window(&k, c, JOIST_WIN_READ, lo, 10) == JOIST_E_STATE);
    JT_CHECK(joist_thread_window(&k, JOIST_MAX_THREADS, JOIST_WIN_READ, lo, 10) == JOIST_E_RANGE);
    JT_CHECK(clean());
}

JT_TEST(window_write_lands_only_in_the_window)
{
    world();
    JT_REQUIRE(joist_thread_window(&k, c, JOIST_WIN_WRITE, VA(LO) + 100u, 9000) == JOIST_OK);
    JT_REQUIRE(serving());
    memcpy(snap, mem, sizeof mem);
    uint64_t n = 0;
    uint64_t w0 = k.work;
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, 50, VA(LO) + 10u, 8000, &n) == JOIST_OK && n == 8000);
    JT_CHECK(k.work - w0 <= 8u); /* a piece of a page at a time: at most 4 pages each side */
    int same = 1;
    for (uint64_t i = 0; i < 8000; i++) {
        same &= *byte_at(T, VA(LO) + 150u + i) == *byte_at(root, VA(LO) + 10u + i);
    }
    JT_CHECK(same);
    /* Nothing else changed: put the copied bytes back and compare. */
    for (uint64_t i = 0; i < 8000; i++) {
        uint8_t *b = byte_at(T, VA(LO) + 150u + i);
        size_t at = (size_t)(b - &mem[0][0]);
        *b = (&snap[0][0])[at];
    }
    JT_CHECK(memcmp(snap, mem, sizeof mem) == 0);
    JT_CHECK(k.th[s].reply_to == c + 1u); /* the reply is still held */
    JT_CHECK(clean());
}

JT_TEST(window_read_from_any_present_memory)
{
    world();
    /* The plain page (not a region's) may be read. */
    JT_REQUIRE(joist_thread_window(&k, c, JOIST_WIN_READ, VA(PL), JOIST_PAGE_SIZE) == JOIST_OK);
    JT_REQUIRE(serving());
    uint64_t n = 0;
    JT_CHECK(joist_reply_read(&k, s, JOIST_SLOT_NONE, 96, VA(LO) + 3000u, 4000, &n) == JOIST_OK && n == 4000);
    int same = 1;
    for (uint64_t i = 0; i < 4000; i++) {
        same &= *byte_at(root, VA(LO) + 3000u + i) == *byte_at(T, VA(PL) + 96u + i);
    }
    JT_CHECK(same);
    /* Into the server's own plain page: never written (it may be shared). */
    memcpy(snap, mem, sizeof mem);
    JT_CHECK(joist_reply_read(&k, s, JOIST_SLOT_NONE, 0, VA(PL), 16, &n) == JOIST_OK && n == 0);
    JT_CHECK(memcmp(snap, mem, sizeof mem) == 0);
    JT_CHECK(clean());
}

JT_TEST(window_copy_stops_at_a_page_it_may_not_touch)
{
    world();
    JT_REQUIRE(joist_thread_window(&k, c, JOIST_WIN_WRITE, VA(LO), VA(8)) == JOIST_OK);
    JT_REQUIRE(serving());
    uint64_t n = 0;
    /* Pages 0-5 writable, 6 read-only: the copy stops at page 6. */
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, VA(5) + 100u, VA(LO), 5000, &n) == JOIST_OK &&
             n == JOIST_PAGE_SIZE - 100u);
    /* Starting on it: nothing. On page 7, not mapped: nothing. */
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, VA(6), VA(LO), 10, &n) == JOIST_OK && n == 0);
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, VA(7), VA(LO), 10, &n) == JOIST_OK && n == 0);
    /* The source not present (the server's page 7): stops there too. */
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, 0, VA(LO + 7u) - 20u, 100, &n) == JOIST_OK && n == 20);
    JT_CHECK(clean());
}

JT_TEST(window_refusals_change_nothing)
{
    world();
    uint64_t n = 77;
    memcpy(snap, mem, sizeof mem);
    /* No reply held. */
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, 0, VA(LO), 10, &n) == JOIST_E_NOREPLY);
    JT_REQUIRE(serving());
    /* No window yet. */
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, 0, VA(LO), 10, &n) == JOIST_E_STATE);
    JT_CHECK(joist_reply_read(&k, s, JOIST_SLOT_NONE, 0, VA(LO), 10, &n) == JOIST_E_STATE);
    JT_CHECK(n == 77 && memcmp(snap, mem, sizeof mem) == 0);
    JT_CHECK(clean());
}

JT_TEST(window_bounds_and_the_copy_limit)
{
    world();
    JT_REQUIRE(joist_thread_window(&k, c, JOIST_WIN_WRITE, VA(LO), VA(JOIST_PT_ENTRIES)) == JOIST_OK);
    JT_REQUIRE(joist_thread_window(&k, c, JOIST_WIN_READ, VA(LO), 1000) == JOIST_OK);
    JT_REQUIRE(serving());
    memcpy(snap, mem, sizeof mem);
    uint64_t n = 77;
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, 0, VA(LO), JOIST_COPY_MAX + 1u, &n) == JOIST_E_RANGE);
    JT_CHECK(joist_reply_read(&k, s, JOIST_SLOT_NONE, 900, VA(LO), 101, &n) == JOIST_E_RANGE); /* past it */
    JT_CHECK(joist_reply_read(&k, s, JOIST_SLOT_NONE, 1001, VA(LO), 0, &n) == JOIST_E_RANGE);
    JT_CHECK(joist_reply_read(&k, s, JOIST_SLOT_NONE, UINT64_MAX, VA(LO), 2, &n) == JOIST_E_RANGE);
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, 0, UINT64_MAX - 3u, 8, &n) == JOIST_E_RANGE); /* wraps */
    JT_CHECK(n == 77 && memcmp(snap, mem, sizeof mem) == 0);
    /* The limit itself is allowed (the copy stops at the read-only page). */
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, 0, VA(LO), JOIST_COPY_MAX, &n) == JOIST_OK &&
             n == VA(6));
    JT_CHECK(joist_reply_read(&k, s, JOIST_SLOT_NONE, 1000, VA(LO), 0, &n) == JOIST_OK && n == 0);
    JT_CHECK(clean());
}

JT_TEST(window_only_while_the_caller_waits)
{
    world();
    JT_REQUIRE(joist_thread_window(&k, c, JOIST_WIN_WRITE, VA(LO), 100) == JOIST_OK);
    /* A caller that times out stops waiting: its window is out of reach. */
    struct joist_msg m = {.r = {1}};
    JT_REQUIRE(joist_recv(&k, s, EP, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_REQUIRE(joist_call(&k, c, T_EP, &m, 10) == JOIST_BLOCKED && k.th[s].reply_to == c + 1u);
    joist_tick(&k, 20);
    JT_REQUIRE(k.th[c].state == JOIST_T_RUNNING);
    memcpy(snap, mem, sizeof mem);
    uint64_t n = 77;
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, 0, VA(LO), 10, &n) == JOIST_E_PEER && n == 77);
    JT_CHECK(k.th[s].reply_to == c + 1u); /* kept: a Reply then gets E_PEER as before */
    /* It calls again: the old reply still can't reach it (another call). */
    JT_REQUIRE(joist_call(&k, c, T_EP, &m, JOIST_FOREVER) == JOIST_BLOCKED);
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, 0, VA(LO), 10, &n) == JOIST_E_PEER);
    JT_CHECK(memcmp(snap, mem, sizeof mem) == 0);
    JT_CHECK(clean());
}

JT_TEST(window_through_a_saved_reply)
{
    world();
    JT_REQUIRE(joist_thread_window(&k, c, JOIST_WIN_WRITE, VA(LO), 100) == JOIST_OK);
    JT_REQUIRE(serving());
    uint64_t n = 0;
    JT_CHECK(joist_reply_write(&k, s, ROBJ, 0, VA(LO), 10, &n) == JOIST_E_NOREPLY); /* unbound */
    JT_REQUIRE(joist_reply_save(&k, s, ROBJ) == JOIST_OK);
    JT_CHECK(joist_reply_write(&k, s, JOIST_SLOT_NONE, 0, VA(LO), 10, &n) == JOIST_E_NOREPLY); /* moved */
    JT_CHECK(joist_reply_write(&k, s, ROBJ, 10, VA(LO) + 500u, 90, &n) == JOIST_OK && n == 90);
    JT_CHECK(*byte_at(T, VA(LO) + 10u) == *byte_at(root, VA(LO) + 500u));
    JT_CHECK(joist_reply_write(&k, s, F_ER, 0, VA(LO), 10, &n) == JOIST_E_TYPE); /* not a reply object */
    JT_CHECK(clean());
}

/* ---- the property: every copy against a byte-by-byte reference ----
 *
 * Random windows over T's region and plain page, random offsets, lengths,
 * sources and destinations in the root, random pages of either side
 * unmapped or made read-only between copies. The reference copies a byte
 * at a time and stops at the first byte whose page may not be read or
 * written, by the test's own rules; the core must copy exactly those bytes
 * and touch no other. */

static uint64_t rs;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static int may_write(joist_cspace sp, uint64_t va)
{
    uint64_t p = va / JOIST_PAGE_SIZE;
    uint64_t e = joist__pte(&k, sp, p);
    return (e & JOIST_PTE_P) && (e & JOIST_PTE_W) && p >= LO && p < LO + JOIST_PT_ENTRIES;
}

static int may_read(joist_cspace sp, uint64_t va)
{
    return (joist__pte(&k, sp, va / JOIST_PAGE_SIZE) & JOIST_PTE_P) != 0;
}

/* An address in one of the 8 region pages or the plain page. */
static uint64_t somewhere(void)
{
    uint64_t p = rnd() % 9u;
    return (p < 8u ? VA(LO + p) : VA(PL)) + rnd() % JOIST_PAGE_SIZE;
}

JT_TEST(window_property)
{
    unsigned copies = 0, partial = 0, refused = 0;
    for (unsigned seed = 1; seed <= 40; seed++) {
        world();
        rs = seed * 0x9e3779b97f4a7c15ull;
        for (unsigned step = 0; step < 50; step++) {
            /* Change a page's mapping now and then (either side's region). */
            if (rnd() % 4u == 0) {
                joist_slot reg = rnd() % 2u ? RT : RR;
                uint64_t p = LO + rnd() % 8u;
                (void)joist_region_unmap(&k, root, reg, p, 1);
                if (rnd() % 2u) {
                    (void)joist_region_map(&k, root, reg, 0, p - LO, 1, p, rnd() % 2u ? JOIST_MAP_W : 0);
                }
            }
            int write = (int)(rnd() % 2u);
            uint64_t wva = write ? VA(LO) + rnd() % VA(8) : somewhere();
            uint64_t wlen = 1u + rnd() % (VA(9));
            if (write && wva + wlen > VA(LO + 8u)) {
                wlen = VA(LO + 8u) - wva;
            }
            /* The caller's window, then its call. */
            if (joist_thread_window(&k, c, write ? JOIST_WIN_WRITE : JOIST_WIN_READ, wva, wlen) != JOIST_OK ||
                !serving()) {
                jt_fail(__FILE__, __LINE__, "a window or a call the test meant to work", 0);
                return;
            }
            uint64_t off = rnd() % (wlen + 2u);
            uint64_t len = rnd() % (VA(3));
            uint64_t mine = rnd() % 2u ? VA(LO) + rnd() % VA(8) : VA(PL) + rnd() % JOIST_PAGE_SIZE;
            memcpy(snap, mem, sizeof mem);
            /* The reference, on a copy of memory: what should change. */
            uint64_t expect = 0;
            int ok = off <= wlen && len <= wlen - off && len <= JOIST_COPY_MAX;
            static uint8_t want[MEM_PAGES][JOIST_PAGE_SIZE];
            memcpy(want, mem, sizeof mem);
            for (uint64_t i = 0; ok && i < len; i++) {
                joist_cspace fs = write ? root : T;
                joist_cspace ts = write ? T : root;
                uint64_t f = write ? mine + i : wva + off + i;
                uint64_t d = write ? wva + off + i : mine + i;
                if (!may_read(fs, f) || !may_write(ts, d)) {
                    break;
                }
                uint8_t *fb = byte_at(fs, f);
                uint8_t *db = byte_at(ts, d);
                (&want[0][0])[db - &mem[0][0]] = (&snap[0][0])[fb - &mem[0][0]];
                expect++;
            }
            uint64_t n = 0;
            joist_err e = write ? joist_reply_write(&k, s, JOIST_SLOT_NONE, off, mine, len, &n)
                                : joist_reply_read(&k, s, JOIST_SLOT_NONE, off, mine, len, &n);
            if ((e == JOIST_OK) != ok || (ok && n != expect) || memcmp(want, mem, sizeof mem) != 0) {
                fprintf(stderr, "  seed %u step %u: %s off %llu len %llu: %d, %llu bytes (expected %llu)\n",
                        seed, step, write ? "write" : "read", (unsigned long long)off,
                        (unsigned long long)len, (int)e, (unsigned long long)n, (unsigned long long)expect);
                jt_fail(__FILE__, __LINE__, "a copy as the reference makes it", 1);
                return;
            }
            copies += ok ? 1u : 0u;
            refused += ok ? 0u : 1u;
            partial += ok && n < len ? 1u : 0u;
            /* The server answers; the caller is free again. */
            struct joist_msg back = {.r = {0}};
            if (joist_reply(&k, s, &back) != JOIST_OK || !clean()) {
                jt_fail(__FILE__, __LINE__, "the reply after the copies, and the invariants", 1);
                return;
            }
        }
    }
    fprintf(stderr, "  window: %u copies (%u stopped at a page), %u refused\n", copies, partial, refused);
    JT_CHECK(copies > 500u && partial > 100u && refused > 100u);
}
