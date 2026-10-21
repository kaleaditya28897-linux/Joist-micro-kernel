# 0021 — Page tables paid for by memory the memory manager donates

Date: 2026-10-21
Status: **accepted** (2026-10-21), with `docs/phase4-plan.md` (choice 3).
Revisits: Phase 3's choice 2 ("donating raw memory to the kernel ... is
reconsidered in Phase 4 with the memory manager",
`docs/phase3-plan.md`).
Covers: §5.1 (no allocation after boot except from memory a task donates),
0003 (no runtime allocation), 0004 (every object paid for), 0013 (all
memory starts as the root task's frames), 0020 (page-sized mappings).

## Context

Today a mapping costs the target task one quota unit, "standing for its
page tables". The tables themselves come from a pool the kernel reserves at
boot, sized for the worst case of the 512-entry mapping table: 1 560 frames
(`POOL_FRAMES`, `kernel/arch/x86_64/run.c`). That worked while a mapping
was a whole frame and there were at most 512 of them.

With 0020 the unit is the page, and the record is the page tables plus a
shadow page per page table. One fully used 1 GiB space needs about 514
page-table pages and 512 shadow pages, about 4 MiB. Fork trees multiply
it. The amount depends on how tasks use memory, so a compile-time pool is
either too small or wastes memory: kernel memory exhaustion, which §1.1
ranks first to avoid. A quota unit per mapping no longer stands for
anything real.

The plan already allows the alternative: §5.1 permits allocation "from
memory explicitly donated by a task".

## Options considered

1. **Honest quotas, a fixed pool.** One quota unit per page-table or shadow
   page actually used, from a pool that stays a compile-time constant
   (`docs/phase4-plan.md`, choice 10). Simple, but the kernel still chooses
   one global limit for everyone.
2. **Memory donated to a space** (this decision). `mm`, which owns the
   memory, lends frames to the spaces it manages, and their page tables come
   from those frames.
3. **The caller of `map` pays from its quota.** Every map by `mm` charges
   `mm`, which becomes one global bottleneck, and freeing needs a refund
   across spaces.
4. **seL4-style untyped memory** for every kernel object, not just page
   tables. More general, but it changes how every object is made, against a
   model and proofs built on quotas (0004). Out of proportion for Phase 4.

## Decision

**B.**

- **`space_donate(task, frame)`** lends a frame to a space. It needs
  CONTROL on the task and CONTROL on the frame. The frame must not be mapped
  anywhere (map count 0, 0020), not dying, and not already lent.
- **While lent**, the frame can't be mapped or split. The kernel holds a
  reference to it, so deleting the donor's capabilities does not free it.
  Its pages are a free list belonging to that space only.
- **Each page-table page the space needs comes from its donated pages:**
  page tables at every level below the top, and a shadow page for each
  last-level page table (0020). A page is zeroed when it is taken. The
  space's top-level table, and the root task's tables at boot, still come
  from the boot pool, which shrinks to one table per space plus the
  root's (a compile-time constant).
- **A `map` whose space has too few donated pages fails with `E_NOMEM`**
  and changes nothing. The kernel never allocates. `mm` donates ahead: when
  it makes a task, and before it maps beyond what it donated. It knows its
  own layout, and `map_query` and the kernel's counters show what remains.
- **Pages are not given back one by one.** A page table emptied by
  `unmap_range` goes back to its space's free list, not to the frame's
  owner. When the space dies its tables go back to their lent frames, the
  kernel drops its references, and each frame is an ordinary frame of its
  owner again. If no capability names it any more, it is freed as any frame
  is.
- **Mappings no longer cost quota units.** Objects (frames, tasks, threads,
  endpoints, regions) stay on quota units, as 0004 has them.
- **Memory is conserved by construction** (I3): a page-table page is
  always inside a frame object, lent or not. The invariant checker adds:
  every page-table and shadow page of a space lies in a frame lent to that
  space, or in the boot pool, and no lent frame is mapped.
- **Fallback.** If donation proves too much for Phase 4, option 1 is built
  instead and this decision is amended to say so.

## Consequences

- The kernel's own memory no longer grows with how much user memory is
  mapped. Who holds memory pays for its page tables, and `mm` holds it all.
- **The safety-critical rule:** a lent frame must never be mapped, or a
  task could write its own page tables. `space_donate` refuses a mapped
  frame, `map` refuses a lent one, and the invariant checks both. CBMC's
  harness and a mutation that drops either check must show it.
- `mm` must donate before mapping, so its allocator gains a "page tables
  remaining" figure per task. A miscount is an `E_NOMEM` at `map`, which
  `mm` handles by donating and retrying, never a kernel failure.
- A task that shrinks keeps its emptied page tables until it dies. A call to
  return a lent frame's unused pages can come later, if measured
  memory use asks for it.
- Phase 4's capacities stay compile-time constants (the owner, 2026-10-21).
  Only the page-table pool's size changes meaning: a table per space, not
  the worst case of a mapping table.
- The model's quota accounting for mappings, the growth test's counters and
  `debug_counters` change: free boot frames, and lent pages free and used
  per space.

## Priority check

- **Reliability first:** no kernel memory is exhausted by user behaviour,
  every call stays bounded, and memory stays conserved (I3).
- **Resilience:** `mm` is the only memory owner, as class C needs. A
  restarted `mm` must learn what it lent to each space; the call for that
  (a bounded listing of a space's lent frames, to a holder of the task with
  CONTROL) is designed with `map_query` and built when the restart is
  (Phase 9).
- **POSIX and Reuse** are not affected.
- **Performance:** one call (`space_donate`) per few hundred page tables,
  outside any fault path once a task is set up.
