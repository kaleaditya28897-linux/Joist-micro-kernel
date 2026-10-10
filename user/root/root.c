/* root.c - the root task (decision 0013), as far as Phase 3 step 4 needs
 * it: it is handed all the memory and the boot modules as frame
 * capabilities, and its job is to partition and delegate. It loads each
 * module as a task of its own: validates the ELF image (the kernel's own
 * validator, built for user mode), creates the task, builds its memory a
 * page at a time from the pool (each page mapped here to be filled, then
 * into the task), hands it its capabilities, and starts its thread.
 *
 * Here: module 0 is pong, which gets the endpoint to receive on; module 1 is
 * ping, which gets a badged send capability to it and one to the endpoint
 * the root waits on for ping's report. */
#include "../lib/joist.h"
#include "elf.h"

#define MOD_VA      0x10000000ull /* modules, mapped read-only, 2 MiB apart */
#define SCRATCH_VA  0x20000000ull /* one page being filled */
#define CHILD_LO    0x400000ull
#define USER_TOP    0x0000800000000000ull
#define STACK_PAGES 4u
#define DONATION    64u

static uint64_t next_slot;
static uint64_t pool_cur, pool_left, pool_next, pool_end;

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free);

static __attribute__((noreturn)) void die(const char *why)
{
    j_puts("root: ");
    j_puts(why);
    j_puts("\n");
    j_exit(1);
}

static void must(uint64_t e, const char *what)
{
    if (e != J_OK) {
        j_puts("root: ");
        j_puts(what);
        j_puts(" failed with ");
        j_putu(e);
        j_puts("\n");
        j_exit(1);
    }
}

/* One page of memory, as a frame of its own: split off the pool. */
static uint64_t page_alloc(void)
{
    while (pool_left == 0) {
        if (pool_next == pool_end) {
            die("out of memory");
        }
        pool_cur = pool_next++;
        pool_left = j_frame_pages(pool_cur);
    }
    if (pool_left == 1) {
        pool_left = 0;
        return pool_cur;
    }
    uint64_t rest = next_slot++;
    must(j_frame_split(pool_cur, 1, rest), "frame_split");
    uint64_t page = pool_cur;
    pool_cur = rest;
    pool_left--;
    return page;
}

/* A fresh page for the task in tslot at va: zeroed, with `n` bytes from
 * `from` copied to offset `off`, then mapped there with perms. */
static void give_page(uint64_t tslot, uint64_t va, unsigned perms, const uint8_t *from,
                      uint64_t off, uint64_t n)
{
    uint64_t f = page_alloc();
    uint8_t *p = (uint8_t *)(uintptr_t)SCRATCH_VA;
    must(j_map(f, J_SLOT_SELF, SCRATCH_VA / J_PAGE, J_MAP_W), "map (scratch)");
    for (uint64_t i = 0; i < J_PAGE; i++) {
        p[i] = 0; /* pool memory is whatever boot left there */
    }
    for (uint64_t i = 0; i < n; i++) {
        p[off + i] = from[i];
    }
    must(j_unmap(J_SLOT_SELF, SCRATCH_VA / J_PAGE), "unmap (scratch)");
    must(j_map(f, tslot, va / J_PAGE, perms), "map (task)");
}

/* Load module `index` (frame in mslot) as a new task in tslot; its entry. */
static uint64_t load(uint64_t index, uint64_t mslot, uint64_t tslot)
{
    uint64_t pages = j_frame_pages(mslot);
    if (pages == 0 || pages > J_MAP_SPAN) {
        die("a module isn't a frame of at most 512 pages");
    }
    const uint8_t *img = (const uint8_t *)(uintptr_t)(MOD_VA + index * 0x200000ull);
    must(j_map(mslot, J_SLOT_SELF, (uint64_t)(uintptr_t)img / J_PAGE, 0), "map (module)");
    struct elf_image im;
    if (elf_validate(img, pages * J_PAGE, CHILD_LO, USER_TOP, &im) != KERR_OK) {
        die("a module was refused by the ELF validator");
    }
    must(j_task_create(DONATION, tslot), "task_create");
    for (uint32_t i = 0; i < im.nsegments; i++) {
        const struct elf_segment *s = &im.seg[i];
        unsigned perms = ((s->flags & ELF_SEG_W) ? J_MAP_W : 0u) | ((s->flags & ELF_SEG_X) ? J_MAP_X : 0u);
        uint64_t first = s->vaddr & ~(uint64_t)(J_PAGE - 1u);
        for (uint64_t page = first; page < s->vaddr + s->memsz; page += J_PAGE) {
            /* The bytes of [vaddr, vaddr + filesz) that fall in this page. */
            uint64_t lo = page > s->vaddr ? page : s->vaddr;
            uint64_t hi = page + J_PAGE < s->vaddr + s->filesz ? page + J_PAGE : s->vaddr + s->filesz;
            uint64_t n = hi > lo ? hi - lo : 0;
            give_page(tslot, page, perms, img + s->offset + (lo - s->vaddr), lo - page, n);
        }
    }
    for (unsigned i = 0; i < STACK_PAGES; i++) {
        give_page(tslot, CHILD_LO - (uint64_t)(i + 1u) * J_PAGE, J_MAP_W, img, 0, 0);
    }
    return im.entry;
}

void _start(uint64_t pool_first, uint64_t pool_n, uint64_t mod_first, uint64_t nmods,
            uint64_t first_free)
{
    next_slot = first_free;
    pool_next = pool_first;
    pool_end = pool_first + pool_n;
    if (nmods < 2) {
        die("needs two modules: pong and ping");
    }
    uint64_t pong_task = next_slot++;
    uint64_t ping_task = next_slot++;
    uint64_t pong_entry = load(0, mod_first, pong_task);
    uint64_t ping_entry = load(1, mod_first + 1u, ping_task);

    /* The endpoint pong serves, and the one ping reports on. */
    uint64_t ep = next_slot++;
    uint64_t done = next_slot++;
    uint64_t tmp = next_slot++;
    must(j_obj_create(J_ENDPOINT, ep), "obj_create");
    must(j_obj_create(J_ENDPOINT, done), "obj_create");
    must(j_cap_give(pong_task, ep, 0, J_R_RECV), "cap_give (pong's endpoint)");
    must(j_cap_mint(ep, tmp, J_R_SEND | J_R_GRANT, 7), "cap_mint");
    must(j_cap_give(ping_task, tmp, 0, J_R_SEND), "cap_give (ping's endpoint)");
    must(j_cap_delete(tmp), "cap_delete");
    must(j_cap_mint(done, tmp, J_R_SEND | J_R_GRANT, 1), "cap_mint");
    must(j_cap_give(ping_task, tmp, 1, J_R_SEND), "cap_give (ping's report)");
    must(j_cap_delete(tmp), "cap_delete");

    must(j_thread_spawn(pong_task, next_slot++, pong_entry, CHILD_LO), "thread_spawn (pong)");
    must(j_thread_spawn(ping_task, next_slot++, ping_entry, CHILD_LO), "thread_spawn (ping)");
    j_puts("root: pong and ping started\n");

    struct j_msg m = {0};
    must(j_recv(done, &m, J_FOREVER), "recv (report)");
    j_puts("root: ping reported ");
    j_putu(m.r[0]);
    j_puts(" round trips\n");
    m.caps = 0;
    (void)j_reply(&m);
    j_exit(0);
}
