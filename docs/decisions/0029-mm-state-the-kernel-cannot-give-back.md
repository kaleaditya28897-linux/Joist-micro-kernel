# 0029 — `mm`'s restart: the records the kernel can't give back

Date: 2026-10-30
Status: **accepted** (the owner, 2026-10-30)
Corrects: 0023's restart note ("rebuilds its records from what it holds
... and `map_query`"), and with it the Phase 4 exit row "`mm` restartable
by design" (`docs/phase4-exit.md`), which rests on that note.
Covers: §7.2 (class C: rebuild from kernel + DS + client
re-registration), §9.2 (state read back is untrusted input), 0020
(`map_query`), 0021 (lent pages), 0023, 0024 (`MM_NONE`), 0028 (trees).
Blocks: Phase 11 (§11: "make proc, auth, exec, memory manager restartable
(class C)"). Recorded now because Phase 4's exit counts the design as done.
0020, 0021, 0023 and `docs/phase4-exit.md` say `mm`'s restart is Phase
9's. §11 gives Phase 9 RS, DS and classes A and B, and class C to Phase 11.
This entry follows §11. Phase 9's RS is what will hold the state frames.

## Context

0023 says a restarted `mm` rebuilds its records from its task
capabilities, its frames, and `map_query`. Phase 4's exit took that as
"restartable by design: everything it needs is readable back from the
kernel".

Reading `servers/mm/mmlib.h` against what the kernel records shows three
things that are only in `mm`:

1. **Regions with nothing mapped.** `struct mm_region` (start, length,
   permissions, shared) exists from `MM_ALLOC` on. The kernel learns of a
   region only when a page in it is first faulted. A region allocated but
   not yet touched leaves no trace, so after a restart, the first touch
   would be refused as "outside every region".
2. **Pages kept but unmapped.** `MM_NONE` (0024) removes the kernel's
   mappings and keeps the pages' contents. `mm` still holds those pages in
   its `map` table. `map_query` shows nothing for them, and the frames `mm`
   holds don't say which task or address they belong to. A guard page or a
   `PROT_NONE` range would come back empty, its contents lost.
3. **A region's permission, as opposed to a page's.** A copy-on-write page
   is mapped read-only inside a writable region. In `map_query`, a
   read-only region and a writable region whose pages are shared look the
   same. So would a `shared` region and a COW one with a single mapper.

0028 adds a fourth (tree membership and accounts). §7.2 already says class
C servers rebuild from "kernel + DS + client re-registration", so the
kernel alone was never going to be enough. The record just claimed more
than the design gives.

## Options considered

**A. The kernel records regions.** Rejected: regions, permissions and
copy-on-write are `mm`'s policy (0008, §5.7).

**B. Clients re-register their regions** with a new `mm`. Clients are
untrusted (a task could claim memory it never had). A task without glibc
couldn't. And glibc gains a recovery protocol on its hottest path.

**C. `mm` writes its records to DS.** That is the plan's general answer, and
DS's rules fit (opaque blobs, per-publisher, nothing secret). But every
`mmap`, `mprotect` and fork becomes an extra IPC to DS, on the path 0008
says must be fast. And `mm` can't restart before DS is up.

**D. `mm`'s records live in frames that outlive it.** `mm` keeps its tables
(already fixed-size and built from indices, not pointers: `mm_page.next`,
`mm_map.page`) in frames the restarter also holds a capability to. A new
`mm` is handed the same frames, **validates every record against the
kernel**, and resumes. No IPC per change, and no dependency on DS.

## Decision

**D, with validation as the rule that makes it safe.**

- **The state frames.** At `mm`'s start, the root (from Phase 9, RS) gives `mm` the
  frames for its records and keeps its own capability to them, without
  WRITE and without KILL (0022). Only `mm` writes to them.
- **Write order.** `mm` updates its records so that a crash at any point
  leaves each task's records either before or after the change (one
  generation word per task record, written last).
- **Validation on restart** (§9.2: what's read back is untrusted input).
  For each task record: its task capability is live; every mapped page in
  the record matches `map_query`; every frame map count matches the
  records' total; every unmapped kept page (2, above) is in a frame `mm`
  holds and isn't mapped anywhere; region permissions bound every mapped
  page's permissions; tree counts match their tasks. A task whose records
  fail any check is killed. A guess is never repaired. The other tasks go
  on.
- **What validation can't catch:** a record that is wrong but consistent
  with the kernel (for example, a region's length). The write order is
  what guards that, and the restart test (below) checks it.
- **Lent pages (0021):** the call 0021 already plans, for a restarted `mm`
  to learn what it lent each space, is part of this work.
- **DS stays the answer for other class C servers.** `mm` is the exception,
  because its state changes on the fast path.

## Consequences

- The Phase 4 exit row reads "partly designed": pages that are mapped are
  readable back from the kernel. Untouched regions, kept-unmapped pages and
  region permissions are not; this entry settles them. The exit record
  itself is left as accepted, and this entry is its correction.
- `mm`'s tables move from its own memory into the state frames. Their
  layout is already position-independent.
- Each record change costs one more write (the generation word).
- A crash that corrupts the records of some tasks costs those tasks, not
  the system.

## Verification (for Phase 11)

- `mmlib`: unit tests that crash at every write of each operation (a fault
  injector in the host build) and restart from the frames. Each task must
  be either fully before or fully after, or killed by validation, never
  silently wrong.
- **Mutations:** a validation check removed; the generation written first;
  a kept-unmapped page not checked against the map counts.
- **The kernel:** `cowstress` with `mm` killed at random and restarted by
  the root; every surviving task's memory compared with a shadow copy.

## Priority check

- **Reliability:** nothing on the fast path waits on another server.
- **Resilience:** makes `mm`'s class C restart possible without trusting
  clients, and without needing DS first.
- **POSIX:** `PROT_NONE` and untouched mappings survive a restart.
- **Reuse:** none affected.
- **Performance:** one extra write per record change. No extra IPC.
