# 0020 — Page-sized mappings: part of a frame, the page tables as the record

Date: 2026-10-21
Status: **accepted** (2026-10-21), with `docs/phase4-plan.md` (choice 1);
**amended** 2026-10-27 (how `map_query` names the frame, below).
Revises: the mapping half of `docs/abi.md`, *Frames and mappings*, and
invariant I12 (Phase 3 step 3).
Covers: §5.5 (the kernel manages only page tables and frame capabilities),
0008 (`map`, `unmap`, `protect`), 0011 (COW by `mm` through the fault path),
0003 (bounded work, no allocation), 0006 (Phase 5's move-grant).

## Context

Phase 4's exit is a `fork` by the memory manager with copy-on-write under
stress. COW needs control of single pages. After `fork`, a page shared
read-only by parent and child must become a private, writable copy in
whichever task writes it first, while the other keeps the original.

What Phase 3 built can't do this:

- A mapping is a **whole frame**, at most 512 pages, inside one page table
  (`joist_map`, `kernel/core/map.c`; I12: "every mapping maps a whole live
  frame").
- `frame_split` refuses a frame that is mapped or that has more than one
  capability, so a frame can't be cut into pages once it has been shared.
- Mappings live in one global table of 512 entries in the kernel build
  (`JOIST_MAX_MAPPINGS`), scanned whole by `map` and `unmap`. Frames are
  objects in a global table of 1 024 (to be 2 048, `docs/phase4-plan.md`
  choice 10), and making one scans it.

For one fully used 1 GiB address space (262 144 pages of 4 KiB), with the
record sizes in today's tree (an object 128 B, a capability 16 B, a mapping
32 B):

| | A: one frame per page | B: part of a frame (this decision) | C: a capability per mapping (seL4's shape) |
|---|---|---|---|
| Frame objects | 262 144 (32 MiB), against a table of 2 048 | a few hundred large frames | as A, unless capabilities held addresses, which Joist's don't |
| Capability slots in `mm` | 262 144, against 8 192 per space | a few hundred | 262 144 per space mapped, twice after `fork` |
| Mapping records | 262 144 (8 MiB), against 512 | none: the page tables, plus a shadow page per page table (about 2 MiB per GiB mapped) | none: in the capability |
| Cost per call | three tables scanned at this size | the page entries written, at most three table pages added | O(1) per page; nothing batched |

## Options considered

1. **One frame per page.** `mm` splits its memory into single pages before
   mapping any. This needs no new kernel mechanism, but multiplies three
   global tables by about 256 and the scans with them.
2. **Map part of a frame, the page tables as the record** (this decision).
3. **A capability per mapping**, seL4's way: each mapped page is a
   capability that records where it is mapped. The kernel record is small,
   but Joist's slot limit and per-object record make this the most
   expensive here.
4. **A kernel page object with COW in the kernel** (Zircon's VMOs, Mach's
   shadow objects). This contradicts §5.5 and 0011 (COW belongs to `mm`),
   and Mach's shadow chains are what the plan sets out to leave behind.

## Decision

**B.**

- **`map(frame, page offset, n, task, va, perms)`** maps pages
  `[offset, offset + n)` of the frame at `va`. `n ≥ 1`, the run lies inside
  the frame and inside one page table (`JOIST_MAP_SPAN`, 512 pages), and it
  needs CONTROL on the frame, plus WRITE for a writable mapping. W^X is
  checked per call. Each page in the run must be absent, unless the call
  asks to **replace** (below).
- **The global mapping table goes.** The record of what is mapped is the
  page tables themselves, plus one **shadow page** per page-table page.
  Each shadow entry gives, for its page entry, the frame object it names
  (32 bits) and its **rights ceiling**: whether the frame capability used
  allowed writing. The page offset within the frame follows from the
  physical address. Shadow pages are paid for like page tables
  (decision 0021).
- **Each frame keeps a map count**: the number of present page entries
  naming it.
- **A conditional replace.** With `REPLACE`, the call may overwrite
  present entries, but only if every page in the run holds what the caller
  says it expects: absent, or a given frame and page offset. Otherwise it
  fails with `E_STATE` and changes nothing. A replace does one TLB shootdown
  for the run, to the CPUs that may hold the space. This lets `mm` resolve
  a fault without locks of its own around the kernel call, and it rules out
  Dirty COW's kind of race: a discard of the page racing the copy for a
  write fault.
- **`unmap_range(task, va, n)`** clears the present entries in a run
  (holes allowed); one shootdown. It replaces `unmap(va)`.
- **`protect(task, va, n, perms)`** sets the permissions of the present
  entries in a run, up to each page's rights ceiling, W^X checked; one
  shootdown, or none for an upgrade (a stale read-only entry only faults
  again).
- **`map_query(task, va, n ≤ 64, buffer)`** returns, per page: present or
  not, the permissions, the page offset, and the frame, but the frame only
  if the caller holds that frame with CONTROL. It never returns a physical
  address. It lets `mm`'s view be checked against the kernel's (0008) and
  rebuilt after a restart (class C, Phase 9).
- **No kernel copy or zero call.** `mm` keeps the memory it manages mapped
  in its own space, read-write and never executable, and copies and zeroes
  pages itself (§9.1 already has `mm` reading and writing all user memory).
- **Freeing.** Deleting a frame's last capability frees it at once when its
  map count is 0. Otherwise the frame is **dying**: no new mapping of it
  is allowed, and a sweep of every space's shadow pages, one page table per
  step, joins the destruction work list (0016's preemption points). It is
  freed when its count reaches 0. A dying space's pages go the same way,
  with its page tables.
- **I12 becomes:** frames are disjoint and non-empty; every present user
  page entry names a page inside a live or dying frame, as its shadow
  entry says; the map counts add up to the present entries; no entry is
  writable beyond its rights ceiling; W^X; user half only.
- **The `SELF` gap is closed.** Mapping into, unmapping from or protecting
  one's own space needs a capability to one's own task with CONTROL, like
  any other task (`docs/phase4-plan.md`, choice 2).

## Consequences

- `mm` manages a few hundred large frames, allocates pages from them, and
  keeps the COW reference counts and owners itself. The kernel knows only
  which frame each page entry names and how many name each frame.
- Kernel tables no longer grow with the amount of mapped memory: shadow and
  page-table pages are donated (0021), and the object table holds frames,
  not pages.
- A frame freed while still mapped costs a sweep over all spaces. That is
  bounded per step but long overall, and it happens only when `mm` dies or
  errs. If measurements show it matters, the fallback is a reverse map
  threaded through the shadow entries (12-16 B per entry, twice the shadow
  memory), making the sweep O(mappings of that frame).
- One call can now write 512 page entries and shoot down the TLBs, under the
  one lock. It is the longest call so far, and is measured at step 4 of the
  plan. If it is too long, the bound drops below one page table.
- Phase 5 (0006) can move a run of page entries between spaces, since each
  entry names its frame. Only `mm` knows which pages are COW-shared, so
  which pages the kernel may move is fixed then
  (`docs/phase4-plan.md`, choice 9).
- Huge pages (2 MiB mappings) are not part of this; they would need a
  shadow entry per directory entry, and are left until measured.
- The model's mapping table, I12's checker, CBMC's harness and the
  growth test's counters all change with it. The model goes first.

## Amendment (2026-10-27): the caller names the frame `map_query` reports

"The frame only if the caller holds that frame with CONTROL" can't be
checked cheaply: the kernel would search the caller's space (up to 8 192
slots) for every page it reports, or keep a reverse index from objects to
the slots that name them. The owner chose to have the caller name the frame:

- **`map_query(task, va, n ≤ 64, frame_slot, buffer)`** reports, per page:
  absent; or present, its permissions and its rights ceiling, and either
  "the named frame, at page offset *o*" or "another frame". The named
  frame needs CONTROL (no frame at all may be named). It never reveals a
  frame the caller doesn't hold, nor a physical address. O(n), and it
  changes nothing.
- Checking `mm` against the kernel (0008) works directly: `mm` knows which
  frame it mapped at each page and asks.
- **Open, for Phase 9:** rebuilding `mm` after a restart (class C) from
  `map_query` alone takes one pass per frame it holds. Whether `mm` keeps a
  journal instead, or the kernel gains a cheaper way, is settled with
  `mm`'s restart.

Rejected: opaque frame ids for every page (they reveal which pages of
other tasks share a frame, against the rule above), and leaving
`map_query` out of Phase 4 (it drops 0008's check from the real kernel).

## Priority check

- **Reliability first:** every call stays bounded by one page table and
  one shootdown, nothing is allocated at run time, and memory use is paid
  for by donated pages (0021), not by kernel tables that grow.
- **Resilience:** `map_query` lets `mm`'s state be read back from the
  kernel, which its restart (class C, Phase 9) needs.
- **POSIX:** `fork`'s COW becomes possible at all.
- **Reuse** is not affected: the kernel stays Mach-shaped only at `mm`'s
  RPC level, as 0008 intends.
- **Performance last:** a page fault costs one call writing a few page
  entries, not a scan of a global table.
