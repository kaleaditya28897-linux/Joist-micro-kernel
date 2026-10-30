# 0030 — The region capability: a task maps its own pages inside a range `mm` gives it

Date: 2026-10-30
Status: **accepted** (the owner, 2026-10-30, with `docs/phase5-plan.md`;
its shape, several frames a region, chosen by the owner the same day)
Carries out: 0008's escape hatch ("a capability over a range"), designed
as Phase 4's choice 7 (`docs/phase4-plan.md`); built first in Phase 5 (the
owner, 2026-10-30).
Covers: §5.7 (the kernel holds page tables, not policy), 0008, 0020-0022
(page-sized mappings, donated tables, revoke by kill), 0023 and 0028
(`mm`'s records and per-tree limits), 0029 (`mm`'s restart state), 0006
(move-grant, which moves only pages in regions).
Blocks: Phase 5 (built first); Phase 7 (glibc's `malloc` stands on it).

## Context

0008 found the VM calls hot: over 150 000 `vm_map`, `vm_deallocate`,
`vm_protect` and `vm_allocate` in a build that stops at `configure`, most
of them `malloc` and `free`. Through `mm` each is an RPC: about 1 300
cycles a request on the P53 before any work (Phase 4, `step7-p53`), and a
fault per page touched. 0008 accepted a way round it: `mm` hands a task a
pre-authorised range, inside which the task maps and unmaps by direct
kernel call. It must stay accounted and revocable: "not a hole in the
model".

Phase 4 designed it (choice 7) and left it for after the exit. Phase 4's
choice 9 adds a second use: 0006's move-grant moves pages between tasks,
and only `mm` knows which pages are COW-shared, so the kernel may only
move pages that are never COW-shared. Pages in regions are those.

## Options considered

**A. One frame per region.** A region is a range, one frame (an offset
window in it) and a rights ceiling. Simplest to check: an entry in the
range names a page of that frame. To grow, the holder asks `mm` for a
second region.

**B. Several frames per region** (the owner's choice, 2026-10-30). A
region holds a bounded list of extents, each a window of one frame. `mm`
adds an extent to grow a region in place; the holder maps any page of any
extent anywhere in the range. A `malloc` arena grows without a new range
and a new capability each time.

**C. Regions the holder may pass on** (with GRANT). Sharing without `mm`;
but a passed region is used by a task `mm` never charged, and revoking it
means finding every holder: what 0022 avoids by killing the object.

## Decision

**B, with these bounds and rules.**

- **The object.** A *region* names one task, a page range `[lo, hi)` of
  its user half, at most `JOIST_REGION_EXTENTS` extents (a build-time
  constant; 8 proposed), and a rights ceiling (read, write, execute; W^X
  holds per page as everywhere). Each extent: a frame, a first page and a
  page count in it. A task has at most `JOIST_REGIONS_PER_TASK` regions
  (8 proposed), kept in its space's record, so any `map` can check them in
  O(8).
- **The range.** Whole page tables: `lo` and `hi` on 2 MiB boundaries, at
  most `JOIST_REGION_TABLES` tables (32 proposed: 64 MiB). Regions of one
  task don't overlap. Creating one checks that each of its tables is empty
  (a count of present entries per table, kept in its shadow page: O(tables)).
- **Who does what.**

  | Call | Who | What |
  |---|---|---|
  | `region_create(task, lo, tables, ceiling, dst)` | holder of CONTROL on the task (`mm`) | an empty region; the range must be free |
  | `region_add(region, frame, off, n)` → extent | CONTROL on the region and on the frame | a new extent; refused past `JOIST_REGION_EXTENTS`, or a frame that is lent (0021) |
  | `region_map(region, extent, page, n, va, perms)` | MAP on the region | pages of that extent at `va`, inside the range, within the ceiling; one page table a call, as `map` |
  | `region_unmap(region, va, n)` | MAP | as `unmap_range`, inside the range only |
  | `region_protect(region, va, n, perms)` | MAP | as `protect`, inside the range, within the ceiling |
  | `obj_kill(region)` | KILL (`mm`) | the range unmapped (on the destruction work list, a table a step), every capability to it dead |

  `mm` keeps CONTROL, KILL and GRANT, and gives the task MAP alone: the
  holder can't pass it on (rejecting C), add extents or kill it.
- **Plain calls and regions.** `map`, `map_replace`, `unmap_range` and
  `protect` refuse a range that overlaps a region (`E_STATE`): inside a
  region only region calls act. `mm`'s own records leave region ranges
  out (0023).
- **Frames dying.** A frame killed while in an extent (0022) is swept as
  now; its extent is then dead, and `region_map` on it is `E_DEAD`.
  Extents are never removed one by one: to shrink, `mm` makes a new
  region and kills the old.
- **I12, extended.** Every present entry inside a region's range names a
  page of one of its live extents, with permissions within its ceiling;
  no entry outside every region names a page of a region's extent unless
  `mm` mapped it there (the frame is `mm`'s).
- **Accounting (0028).** A region's object is charged to `mm`'s quota; its
  frames are pages of `mm`'s pool, counted against the task's tree when
  `region_add` is made. Page tables come from the task's donated pages
  (0021), as for any `map`.
- **Fork (0023).** The child gets new regions of the same shape, with new
  frames, and `mm` copies the pages. Region pages are never COW-shared.
- **Restart (0029).** A region and its extents are read back from the
  kernel (the region's record, `map_query`); `mm`'s shared state adds only
  which tree each region's frames are charged to.

## Consequences

- **Easier:** `malloc` and `mmap(MAP_ANONYMOUS)` without an RPC each;
  0006's move-grant has pages it may safely move; a receive window for
  Phase 5's bulk transfers.
- **Harder:** a second writer of a task's mappings (0008's warning). I12's
  extension and the refinement test (a task's region pages are its own)
  keep the two apart. A new object type, five calls and a per-table count
  in the kernel: about 400-600 lines, to be counted.
- **Open, settled in the model step:** whether the per-table count costs
  too much on every map and unmap; if so, creation checks the range by a
  preemptible sweep instead.

## Priority check

- **Reliability:** every call bounded by one page table, or by
  `JOIST_REGION_TABLES` at creation; no kernel allocation (donated tables,
  fixed-size records); revocable in one call.
- **Resilience:** recoverable from the kernel at `mm`'s restart.
- **POSIX:** what `malloc` and anonymous `mmap` need.
- **Reuse:** none lost: glibc's allocator calls through its sysdeps.
- **Performance:** the reason it exists; measured against RPCs to `mm`
  before Phase 5's exit, as 0008 asks. Performance does not buy anything
  from 1-4 here.
