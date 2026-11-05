# Phase 5 plan: the region capability, MIG retarget and libports

**Status: accepted by the project owner, 2026-10-30**, with decisions 0030
(the region capability, several frames a region) and 0031 (the reused Hurd
code on glibc from this phase), and the numbers they propose.

Phase 5 of §11: *MIG retarget and libports*, with two items the owner
added: the region capability first (2026-10-30), and `mm`'s limits per
task tree (decision 0028).

## The goals this plan answers to

§1.1, in order. Every choice below ends with how it ranks against them.

1. **Reliability (QNX):** no unbounded behaviour, no priority inversion,
   no kernel memory exhaustion. Phase 5 brings the first reused Hurd code
   onto Joist. Reused code that breaks a bound is changed or not used.
2. **Resilience (Minix 3):** every server has a crash and restart story.
   libports is the core of every Hurd server; what it does when a client
   or a server dies is designed here, not found later.
3. **POSIX:** full conformance is the target. Phase 5 starts glibc on
   Joist (0031), so the reused servers run on the real libc; the region
   capability is what its `malloc` stands on (0008), and the MIG paths are
   what every POSIX call will cross.
4. **Reuse of the Hurd:** MIG's `.defs`, libports' interface, libtrivfs and
   a first translator, kept as they are where 1-3 allow.
5. **Performance:** the region capability against RPCs to `mm` (0008's
   measurement), and the three OOL paths of 0006.

## Inputs

- **The plan's Phase 5** (`joist-microkernel-plan.md` §11): a MIG backend
  for native IPC with `.defs` unchanged; libports on native capabilities
  and lifetime notifications; a trivial translator (libtrivfs "hello");
  the region capability first; `mm`'s per-tree limits.
  **Exit:** unmodified-logic Hurd server code runs on the new kernel via
  retargeted stubs; the region capability built, its calls in the model
  with CBMC and mutations, and measured against plain RPCs to `mm`; a fork
  bomb and a memory hog, each in its own tree, leave a third task able to
  fault new pages and fork.
- **Decisions:** 0001 (synchronous IPC, 2 capabilities a message, the
  4 KiB IPC buffer), 0005 (port sets as badges on one endpoint), 0006 (OOL
  as three transfers: inline, copy into a receiver's buffer, move-grant),
  0008 (the escape hatch: a capability over a range), 0022 (revoke by
  kill), 0023 and 0027-0029 (`mm`, fault routing, per-tree limits,
  restart state), 0002 (Clang for everything Joist owns; the Hurd's
  nested functions are a cost of their own).
- **Phase 4's design of the region capability** (`docs/phase4-plan.md`,
  choices 7 and 9): a range of one task's addresses, a backing frame and a
  rights ceiling; its holder maps, unmaps and protects pages of that frame
  inside the range, and nothing else; `mm` keeps the frame and can kill
  the region. Pages in a region are never COW-shared (`mm` copies them at
  fork), so the kernel may move them (0006's move-grant) without handing a
  fork child's page to someone else.
- **Carried from Phase 4's exit:** CBMC for `obj_kill` and `cap_delete`
  from states with mapped frames; a test aimed at the sweep's top-level
  stride (`map-sweep-skips-top`). Both touch the code the region
  capability changes, so they belong here.
- **Phase 0's data:** 140 OOL parameters, 54 with `dealloc`
  (`docs/phase0-data/ool-params.csv`); the Hurd's nested functions
  (`nested-functions.csv`): libports 4, all in `manage-multithread.c` and
  `manage-one-thread.c`, the request loops this phase rewrites anyway;
  libtrivfs 1 (`startup.c`, a SIGTERM handler).

## Scope

| In Phase 5 | Not until later |
|---|---|
| The region capability: kernel object and calls, `mm` making and killing regions, measured (0008) | glibc's `malloc` on it (Phase 7) |
| `mm`'s limits per task tree (0028) | `proc` setting them (Phase 8) |
| A MIG backend for Joist's IPC; `.defs` unchanged | — |
| OOL: inline copy and copy through the IPC buffer; move-grant through a receive window (0006) | copy into a receiver's buffer above 4 KiB, if measured necessary |
| libports on endpoints, badges and lifetime notifications | — |
| glibc's `sysdeps/joist`, static, for what the reused servers call: the Mach layer, `htl` threads, `malloc` over a region (0031) | `fork`, `exec`, signals, dynamic linking, `posix_spawn`, client recovery (Phase 7) |
| libtrivfs and the "hello" translator, started by the root and called by a test client | a filesystem namespace, `settrans` (Phase 8) |
| The review of the 54 `dealloc` OOL sites (0006: "the Phase 5 work") | — |
| CBMC for `obj_kill`/`cap_delete` with mapped frames; the sweep's stride test | — |

## Choices this plan makes (proposed)

1. **The region capability (decision 0030, proposed; several frames a
   region, the owner's choice).** A new kernel object: one task, a range
   of whole page tables (2 MiB aligned, at most 32: 64 MiB), up to 8
   extents (each a window of one frame), and a rights ceiling.
   - **`mm`** makes it (`region_create`), grows it (`region_add`, a new
     extent) and kills it (0022). It keeps CONTROL, KILL and GRANT.
   - **The task** gets MAP alone: `region_map`, `region_unmap`,
     `region_protect`, each inside the range, within the ceiling, bounded
     by one page table; it can't pass the region on.
   - **Plain calls** (`map`, `unmap_range`, `protect`) refuse a range that
     overlaps a region, so inside one only region calls act.
   - **I12, extended;** accounting to the tree (0028); fork copies region
     pages (never COW-shared); restart reads regions back from the
     kernel (0029). Details and the one open point (a per-table entry
     count, or a sweep at creation) in 0030.
   - **Measured:** a `malloc`-like churn (map, touch, unmap, 1 to 64
     pages) by region calls against `MM_REQ_MAP_ANON`/`MM_REQ_UNMAP`, on
     the P53.
   - *Priority check:* bounded, accounted, revocable; performance gained
     at no cost to 1-4 (0030).
2. **`mm`'s per-tree limits (0028, accepted):** built after the region
   capability, so that region frames are counted from the start. A fork
   bomb test and a memory hog test, each in its own tree, with a third
   tree that must still fault and fork: Phase 5's exit line.
3. **The MIG backend.** GNU MIG is reused: a new output mode beside its
   Mach one, from the same parser, so `.defs` are read exactly as today.
   - **A message:** the routine's id and its in-line data in the message
     registers and the IPC buffer (0001); port rights as capabilities (up
     to 2 a message; a routine with more is split, or its extra rights go
     through a reply-object exchange; the count is in Phase 0's data, to
     be checked before the backend is written).
   - **Reply ports:** a Mach send-once reply port becomes the call's reply
     capability (0001). A server that answers later (MIG's "no reply now")
     saves it in a reply object.
   - **OOL** (0006): small parameters inline; up to the IPC buffer's
     4 KiB copied through it; the `dealloc` ones by move-grant (choice 4).
     Larger copies without `dealloc` are measured before a fourth path is
     built.
   - **Tests:** marshalling round trips for every routine of `fs.defs`,
     `io.defs` and `fsys.defs` on the host; a fuzzer on the server-side
     demuxer (a malformed message must be refused, never crash it).
   - *Priority check:* reuse first (MIG and `.defs` kept); a message's
     size and its capabilities are bounded as 0001 bounds them.
4. **Move-grant through a receive window (0006, Phase 4's choice 9).** A
   receiver offers a window: a range inside one of its regions. A sender
   moves pages from one of its regions: the kernel unmaps them from the
   sender and maps the same pages into the window, both in one call,
   bounded by one page table. Only pages in regions move, so no
   COW-shared page ever does. Ownership of the frame stays `mm`'s; the
   two regions' records say which pages each holds. *To be written as
   part of 0030 or as its own decision once the region capability is
   built; open until then.*
5. **glibc from this phase (decision 0031, proposed; the owner's
   direction).** The reused servers run on glibc, not on a runtime of
   Joist's own. Phase 5 starts glibc's `sysdeps/joist`, static only, with
   what they call:
   - the Mach layer the Hurd layer stands on: messages through the
     retargeted MIG, `mach_port_*` over capabilities (a name is a slot;
     port sets are badges), `vm_*` over `mm` and regions, tasks and
     threads over Joist's calls;
   - `htl` threads over Joist threads, a scheduling context each;
   - `malloc`'s arenas in a region (choice 1: its first user);
   - startup for a server started before `proc` and `auth`, as the
     Hurd's boot filesystem is; Joist's root plays the boot filesystem.
   - **A pre-committed check:** an inventory first (what these programs
     reach in glibc with `sysdeps/mach` removed). If its estimate passes
     6 weeks, the owner decides on that measurement: go on, narrow the
     scope, or fall back to a runtime of Joist's own.
   - Built with Clang (0002): this is the first cross-build of glibc with
     Clang, 0002's condition 1.
   - **Left in Phase 7:** `fork`, `exec`, signals, dynamic linking,
     `posix_spawn`, client recovery. Phase 7's exit is unchanged.
   - *Priority check:* reuse without writing anything twice; glibc's
     threads made bounded on Joist; `malloc` accounted.
6. **libports on Joist.**
   - A port bucket is one endpoint; each port in it, a badge (0005).
     A port class is libports' own bookkeeping, unchanged.
   - No-senders and dead names come from lifetime notifications (I4),
     collected as `mm` collects them.
   - The request loops (`manage-*.c`) are rewritten for `ReplyRecv`,
     which also removes their nested functions (0002).
   - **Bounded threads:** libports' multithreaded manager makes threads
     on demand. Here it has a fixed pool, set at start, each thread with
     a context; a request that finds none free waits in the endpoint's
     queue, in priority order (§8: libports must not reorder requests
     against priority inheritance).
   - **A dying client:** its reply capability goes, and the server's
     reply fails with `E_PEER`; the request's work is not undone. This is
     what Phase 4's fault injection already checks for the kernel; it is
     checked here for libports.
   - *Priority check:* reliability first (fixed threads, priority order
     kept); resilience (a client's death is ordinary); reuse (libports'
     interface unchanged for its users).
7. **The first translator:** libtrivfs and the Hurd's `trans/hello`,
   unchanged in its logic. The root starts it, hands it a bootstrap
   capability and plays the part of the filesystem that attaches it
   (`fsys_startup`). A test client calls it through MIG user stubs:
   `io_read`, `io_seek`, `io_stat`, `io_write` refused. The one nested
   function in libtrivfs (`startup.c`, a SIGTERM handler) is rewritten.
8. **The 54 `dealloc` OOL sites** (0006): each reviewed for whether the
   sender uses its copy after sending. The result is a table in
   `docs/phase5-data/`, and any site that does gets an explicit copy.

## Testing

| What | How |
|---|---|
| Region calls and I12 extended | the model first: property tests under every sanitizer, "a failed call changes nothing", CBMC on `region_create`, `region_map`, `region_unmap`, `region_protect` and the kill; mutations (a map outside the range, past the window, above the ceiling, a kill that leaves a page) |
| Regions under `mm` | the refinement test's abstract machine gains regions (a task's region pages are its own; fork copies them); `cowstress` gains a region writer |
| Move-grant | property tests: a moved page leaves the sender and appears in the window, never both; never a COW-shared page; mutations |
| Per-tree limits | the refinement test with trees; `mmroot` tests for admission, ceilings and `MM_SET_LIMIT`; the exit's fork bomb and memory hog |
| MIG backend | host round trips for every routine of `fs.defs`, `io.defs`, `fsys.defs`; a fuzzer on the demuxer |
| glibc's `sysdeps/joist` | glibc's own test suite, the subset that runs statically without `fork`, signals or a filesystem; `malloc` stress in a region; `htl` tests (mutexes, condition variables, a bounded pool) on 1, 2 and 4 CPUs |
| libports | a server under fault injection: clients killed mid-call, no-senders exactly once, the thread pool never exceeded |
| The translator | `io_read` and the rest from a test client, on 1, 2 and 4 CPUs, emulated and under KVM; the server killed mid-request |
| Carried from Phase 4 | CBMC for `obj_kill`/`cap_delete` with mapped frames; a test aimed at the sweep's top-level stride |
| Measured, recorded | region churn against RPCs to `mm`; `io_read` by move-grant against copy, 4 KiB to 1 MiB; a MIG round trip against the raw IPC one |
| Phase 4's suites | all of them, unchanged, in every full run |

## Order of work

1. ~~**This plan, and decisions 0030 (region capability) and 0031 (glibc
   from this phase),** accepted by the owner.~~ Done 2026-10-30.
2. ~~**The region capability in the model:** the object, its calls, I12
   extended, property tests, CBMC, mutations; the carried CBMC work.~~
   Done 2026-11-03 (*Progress*).
3. ~~**The region capability in the kernel and `mm`;** measured.~~ Done
   2026-11-03 (*Progress*).
4. ~~**`mm`'s per-tree limits (0028).**~~ Done 2026-11-04 (*Progress*).
5. ~~**The glibc inventory (0031's check):** what libports, libihash,
   libtrivfs, `trans/hello` and a test client reach in glibc without
   `sysdeps/mach`; the estimate; the owner's decision if it passes 6
   weeks. Recorded in `docs/phase5-data/`.~~ Done 2026-11-04: go on with
   glibc (*Progress*).
6. **The MIG backend,** with its host tests (glibc's own Hurd stubs need
   it).
7. **glibc's `sysdeps/joist`, `htl` and `malloc` over a region;** the
   cross-build with Clang.
8. **libports.**
9. **Move-grant** (its decision first), and the `dealloc` review.
10. **libtrivfs and `hello`.**
11. **Exit:** the exit run, the measurements, the record.

## Risks

- **glibc's scope and length.** Phase 5 was 2-3 months; with the glibc
  start it is longer, by an amount the inventory measures. 0031's 6-week
  check makes an overrun a decision, not a drift. glibc's Mach layer may
  assume things Joist doesn't do (asynchronous sends, kernel-buffered
  OOL, port sets moved at run time); each is listed by the inventory
  and mapped or refused.
- **The first Clang cross-build of glibc** (0002, condition 1): a new
  error class re-opens 0002 with a superseding entry.
- **MIG's assumptions.** Routines with more than 2 port rights, `msgh_id`
  ranges, `dealloc` semantics and send-once rights all need a mapping.
  Phase 0's inventory bounds them; any that don't fit are listed before
  the backend is written.
- **libports' threading.** Its on-demand threads are unbounded; a fixed
  pool can deadlock a server that calls itself. Each reused server is
  checked for that, and its pool sized.
- **Two writers of a task's mappings** (0008's warning): the holder in its
  region, `mm` everywhere else. I12's extension and the refinement test
  are what keep them apart.
- **Bare metal** is still not available; every real-CPU figure is under
  KVM on the P53.

## Accepted (2026-10-30)

The plan, decisions 0030 and 0031, and their numbers: 8 extents a region,
8 regions a task, 32 tables a region (64 MiB), and 0031's 6-week check.

## Progress

### Step 2: the region capability in the model (2026-10-30 to 2026-11-02)

- **Built** as 0030 decided: `JOIST_OBJ_REGION`, its record (`struct
  joist_region`, a range of whole last-level tables, up to 8 extents, a
  W/X ceiling) and five calls (`kernel/core/region.c`). `map.c`'s
  page-table work is shared by the plain and region calls. The plain
  calls refuse a run that meets a region (`E_STATE`). A frame in an
  extent is kept (its `extents` count), and can't be split, donated or
  lent. A region's kill unmaps it on the destruction work list, a table a
  step; its task's end releases it at once.
- **0030's open point, settled:** no per-table count. I12 now says there
  is no empty last-level table outside a dying space, so a range with no
  tables has nothing mapped (recorded in 0030).
- **I12 extended** (`joist__check_region`): records, lists, overlap,
  extents counts, every page in a range is in an extent and within the
  ceiling, swept tables empty, and (2026-10-31) no extent names a lent
  frame.
- **Tests:** 13 unit tests (`model/test/test_region.c`); regions in the
  property executor (`JX_REGION`); 262 tests, all six builds of `make -C
  model check` pass. 16 region mutations, and the older map mutations
  rewritten for the shared code: the full run (2026-11-02, joist-dev2,
  at 5b3430d) catches all 301.
- **CBMC:** operations 22-26 (the region calls, with page tables) all
  verify on joist-dev2. Op 24 (`region_map`) first failed: the checker
  accepted a frame both lent for page tables and in an extent, from
  which `region_map` hands a task its own tables. No call reaches it
  (`region_add` refuses a lent frame; lending refuses one in an extent),
  so the checker was weak, not the kernel; it now says so (1935f0f), and
  op 24 verifies (6018 s).
- **Phase 4's carried items:** a test aimed at the sweep's stride (the
  `map-sweep-skips-top` mutant is caught by it directly). CBMC can now
  run any operation with page tables (`OP/t`, or `OP:TYPE/t` for one type
  of capability in the slot acted on). Kill (`9/t`) and delete (`3/t`)
  with page tables don't fit: killed at 11.8 GB on joist-dev2 (12 GB),
  then out of an 18 GB limit on the T490, whole, split by frame and region
  (`9:7/t`, `9:8/t`, `3:7/t`, `3:8/t`), and with CaDiCaL for MiniSAT
  (2026-11-03). **Closed as a known limit** (the owner, 2026-11-03): a
  condemned frame's or region's sweep stays covered by the stride test,
  the property tests and the mutations, as the Makefile already said.
  Fixed on the way: the `cbmc` target's tally never counted a split
  operation (`9/t`, `19/s`, `3:2`) as verified, since it looked for the
  argument, not the name `run.sh` gives its line.

### Step 3: the design of regions in the kernel and `mm` (2026-11-03)

Implementation choices inside 0030, so *(changeable)*.

- **The calls (done, part 1):** system calls 80-84, `region_create`,
  `region_add`, `region_map`, `region_unmap`, `region_protect`, as
  `docs/abi.md` lists them.
- **The kernel's capacity:** `JOIST_MAX_REGIONS` 128 in the kernel build
  (64 spaces, so 2 a task on average; at most 8 for any one).
- **`mm`'s requests**, three, each naming the region by the slot of the
  caller's space that holds it:
  - `MM_REQ_REGION_NEW` (first page on a 512-page boundary, tables and
    permissions, the slot): `mm` makes the region from its own quota and
    gives the caller a capability with MAP alone, in that slot.
  - `MM_REQ_REGION_GROW` (the slot, pages): `mm` takes a run of pages of
    one of its frames, zeroed, as the next extent; the reply gives its
    index. The pages count against the task (`MM_TASK_PAGES`; 0028's
    trees in step 4).
  - `MM_REQ_REGION_FREE` (the slot): `mm` kills the region, runs the
    kernel's pending work until the range is unmapped, then takes its
    pages back. If that work doesn't finish, the pages stay out of the
    pool: lost, never handed out while still mapped. The dead capability
    stays in the task's slot for it to delete.
- **Pages for extents:** an extent is a run of pages of one frame, so `mm`
  looks for a free run in its pages (O(`MM_PAGES`)) and rebuilds its free
  list. Its records mark each region page as its region's, and
  `mm_check` checks that no page is both a region's and free or mapped by
  `mm`.
- **Fork:** the child gets regions of the same shape at the same slots,
  with new extents of the same sizes, the pages copied, and the same
  entries: read from the parent with `map_query`, made with
  `region_map`. Region pages are never COW-shared (0030). The parent's
  copy of its capabilities already steps over a slot the child holds
  (as it does `MM_SLOT`), so it steps over these too.
- **A task gone:** the kernel releases its regions' extents with its
  space, so `mm` takes their pages back with the task's.
- **Tests:** `model/test/test_mm.c` (`mmlib` against the kernel model)
  for the three requests, fork and a task gone; the refinement test's
  abstract machine gains regions (a task's region pages are its own; fork
  copies them); `cowstress` gains a region writer.
- **Measured** on the P53: a churn (map, touch, unmap; 1 to 64 pages) by
  region calls against `MM_REQ_MAP_ANON` and `MM_REQ_UNMAP`.

### Step 3: the region capability in the kernel and `mm` (2026-11-03)

- **Built** as *Step 3: the design* above says: system calls 80-84 with
  the user library's wrappers (part 1, baea6ec); `mm`'s REGION_NEW, _GROW
  and _FREE, a fork's copies at the same slots, a task's end taking its
  regions' pages back, the kernel built with 128 regions (part 2,
  157276b).
- **Tests:** a host test that each call's arguments reach the kernel, 2
  kernel mutations, and the ABI fuzzer listing every call (part 1); 9
  `mmlib` unit tests on the kernel model, 14 mutations (`mm-kreg-*`,
  `mm-anon-over-kreg`), regions in the refinement test's abstract machine,
  `mmroot`'s region test (part 2). `cowstress` gains a region writer
  (part 3, 25dc567): some tasks of the fork trees hold a region, grown
  through `mm` and mapped by the task itself, written by both threads and
  verified after every step with their other pages; a fork's child starts
  with a copy of its own; after each step's operation a second seeded
  stream grows a region (or unmaps a page and maps it back, which must keep
  what it held), frees one, or kills a task while `mm` serves its grow or
  free. Every existing check stays; a new floor needs some grow, some fork
  of a region holder and some remap. In the 20-round runs: 177 grows, 73
  remaps, 23 frees, 204 forks of a region holder, 16 kills mid-request.
  `mm-kreg-clone-not-copied` and `mm-kreg-clone-maps-shared` each fail
  `cow3`.
- **Runs** (joist-dev2): at 157276b, `make -C model check` (270 tests in
  each of the six builds), `make -C model mutate` (315 of 315 caught),
  `make -C kernel check` (55 host tests under plain and four sanitizers,
  87 of 87 self-tests, 32 artifacts reproducible) and `make -C kernel
  mutate` (56 of 56) all pass. With part 3 and 4: `cow3`,
  `kvm-smp1/2/4-cowstress`, `chk-kvm-smp2-cow3` and the three `mmroot`
  runs pass.
- **Measured** (part 4, 2de3585; `docs/phase5-data/step3-region-churn/`):
  map *n* pages, write each, unmap, on the P53 (KVM, 1 CPU, median of 3
  boots' medians), by the task's own region calls against `MM_REQ_MAP_ANON`,
  a fault a page and `MM_REQ_UNMAP`:

  | pages | region, table emptied | requests | region, table kept | requests |
  |---:|---:|---:|---:|---:|
  | 1  | 7 552  | 14 766  | 1 590  | 8 812   |
  | 4  | 9 520  | 31 956  | 3 640  | 25 980  |
  | 16 | 17 786 | 100 260 | 11 844 | 94 040  |
  | 64 | 49 764 | 374 822 | 43 886 | 368 386 |

  The region calls are 2 to 7.5 times cheaper with the table emptied, 5.5
  to 8.4 times with it kept; 2 CPUs alike (up to 8.7).
- **What surprised:** most of a small region churn is the page table, not
  the calls. An unmap that empties the region's table frees it (step 2's
  rule, "no empty last-level table"), and the next map builds and zeroes a
  new one: about 5 950 cycles a cycle, the same on both paths. A 1-page
  region churn is 7 552 cycles with it, 1 590 without (a page kept mapped
  in the table). The first measurement, which only emptied the table, made
  the region calls look only twice as cheap.
- **Decided** (the owner, 2026-11-04, from the hand-off's findings):
  - *A region's tables stay as they are:* an emptied one is freed (I12
    unchanged). An allocator that churns in a table keeps a page mapped in
    it (glibc's `malloc`, step 7), which gives the "table kept" column
    above with no kernel change.
  - *A fork's copy of region pages is documented, not made atomic:* `mm`
    copies a parent's region pages while the parent's other threads may
    still write them (its anonymous pages are made read-only first; region
    pages are not), so a page written during the fork is copied in an
    unspecified state, as POSIX leaves it. The child's copy is its own
    either way. glibc's `fork` takes `malloc`'s locks first, which covers
    its arenas. Recorded in 0030. (`cowstress` keeps a task's helper
    thread idle while it forks, so it doesn't reach this.)
  - Part 2's commit message says 15 new `mm` mutations; there are 14.

### Step 4: the design of `mm`'s per-tree limits (2026-11-04)

Implementation choices inside 0028, so *(changeable)*.

- **Trees.** `mm` keeps a table of trees (`MM_MAX_TASKS` of them: at most
  one a task). An adopted task starts one; a child joins its parent's at
  `MM_REQ_CLONE` (so a pending child counts at once) and stays in it if
  the parent dies; a cancelled clone leaves it. A tree's record: its
  tasks, its pages, its reservation, its page and task ceilings. It lasts
  until it has no tasks and no pages charged to it.
- **Which tree pays for a page.** The tree of the task that made it: a
  demand-zero page, a copy-on-write copy, a page `SHARE` makes, a region's
  extent. A page keeps its tree while any task maps it, so a page shared
  into another tree stays charged to its maker's (a COW share is always
  within one tree). Each page records its tree.
- **The rule for a new page** (`n` pages for a region's extent): refused
  if its tree would pass its ceiling, or if it is past its reservation and
  the pool's free pages, less every tree's unused reservation, are fewer
  than `n`. So `mm` always keeps enough free pages for every tree's unused
  reservation; `mm_check` checks that sum (every tree's larger of pages
  and reservation) never passes the pool.
- **Tasks.** A clone that would take a tree past its task ceiling is
  refused (`MM_E_NOMEM`: `fork`'s `EAGAIN` later).
- **Admission.** `MM_REQ_ADOPT` is refused (`MM_E_NOMEM`) if the new
  tree's reservation doesn't fit beside the others'. A tree with no tasks
  left gives its reservation back at once.
- **Defaults** (build-time): a new tree's reservation `MM_TREE_RESERVE`
  (32 pages), its ceilings half the pool and half the task records.
- **`MM_SET_LIMIT`** (root badge): the tree of an adopted task (named by
  its badge), its reservation and page ceiling (`r[2]`, packed 32 and 32
  bits), its task ceiling (`r[3]`). Refused if the reservation doesn't
  fit; a ceiling below what the tree holds refuses only its next pages.
  The root's tests set their trees' limits with it where the default
  ceiling would change what they test (`cowstress`'s one big tree).
- **Tests** (0028's *Verification*): `mmlib` unit tests (a tree at its
  ceiling, on every path that makes a page; a fork past the task ceiling;
  an adoption refused; a confirmed child kept after its parent dies; a
  pending child counted; a tree's counts returned with its last task); the
  four mutations 0028 names, and more; trees and their counts in the
  refinement test; on the kernel, a fork bomb and a memory hog, each its
  own tree, beside a third tree that keeps faulting and forking and is
  never refused (Phase 5's exit line).

### Step 4: `mm`'s per-tree limits (2026-11-04)

- **Built** as *Step 4: the design* above says (b05da91): `mm`'s table of
  trees in `servers/mm/mmlib.c` (each page charged to its maker's tree,
  the room rule for a new page, admission at `MM_REQ_ADOPT`,
  `mm_task_add_child` joining a child to its parent's tree at
  `MM_REQ_CLONE`, `mm_set_limit`, and `mm_check` checking that the trees'
  claims never pass the pool), and `MM_REQ_SET_LIMIT` in `mm.c` and
  `mm_proto.h`. Defaults: 32 pages reserved (`MM_TREE_RESERVE`), half the
  pool and half the task records. `cowstress`, `mmbench` and `mmroot`'s
  clone-full test set their tree unlimited (`tree_unlimited`, `mmhost.h`):
  what they test is `mm` full, not a tree's limits.
- **Tests:** 5 `mmlib` unit tests (`mm_tree_*`: a tree's ceiling on every
  path that makes a page; its reservation kept for it; admission and
  `SET_LIMIT` refused when the reservations don't fit; children counted
  from `CLONE`, kept after their parent dies, given back with the last
  task; a tree keeping the pages it shared after its tasks go); 11
  mutations (`mm-tree-*`: the four 0028 names (`-ceiling-ignored`,
  `-child-own-tree`, `-tasks-uncounted`, `-admission-unchecked`) and
  `-reserve-ignored`, `-child-ceiling-ignored`, `-page-not-uncharged`,
  `-lost-still-charged`, `-reserve-kept-without-tasks`,
  `-share-room-unchecked`, `-set-limit-unchecked`); trees in the
  refinement test's abstract machine (membership checked every step, a
  refusal only where a tree has no room); `mmroot`'s trees test, Phase 5's exit line against
  the build-time defaults (`MM_MAX_TASKS` 16, a 256-page pool): a fork
  bomb stopped at 7 children (its task ceiling, its children pending), a
  hog at 128 pages (its ceiling, half the pool), a second at 64 (the pool
  less the other trees' reservations), a fifth tree refused at `ADOPT`, a
  reservation raised past what is free refused, and a fourth tree that
  faults and forks beside them never refused ("mm: trees ok").
- **Runs** (joist-dev2, at b05da91): `make -C model check` (275 tests in
  each of the six builds), `make -C model mutate` (326 of 326 caught),
  `make -C kernel check` (55 host tests under plain and four sanitizers;
  tidy and scan-build clean; 87 of 87 self-tests, among them the three
  `mmroot` runs with "mm: trees ok", `cow3`, `kvm-smp1/2/4-cowstress` and
  `chk-kvm-smp2-cow3`; 32 artifacts reproducible) and `make -C kernel
  mutate` (56 of 56) all pass.
- **What surprised:**
  - *`SHARE` changed things before it knew the tree had room.* Writing
    the unit tests found that `mm_share` could refuse for want of room
    after it had already begun to change the sharer's records. It now
    checks the tree's room for every page it will make first, and
    `mm-tree-share-room-unchecked` holds it there (fixed in b05da91).
  - *`mm`'s table runs are a shared resource 0028 didn't name.* `mm`
    lends each task record a run of `MM_TABLE_PAGES` table pages from the
    pages its starter gave it (`mm.c`'s `lend_tables`). With fewer runs
    than task records (`mmhost.h`'s default is 8), a fork bomb spends
    every run while still below its tree's task ceiling, and a new task in
    any other tree is refused, which is the exhaustion 0028 exists to
    prevent. `mmroot` lends `mm` one run per task record
    (`HOST_TABLE_RUNS` = `MM_MAX_TASKS`), so the task ceiling bounds the
    runs too. Whoever starts `mm` (the root, later RS) must do the same,
    or `mm` must count runs per tree. **Decided** (the owner, 2026-11-04):
    `mm` refuses `MM_REQ_INIT` (`MM_E_NOMEM`) unless it is given a run for
    every task record, so its starter can't get it wrong; `mmhost.h` gives
    that by default, and `mmroot`, `cowstress` and `mmbench` no longer set
    their own.

### Step 5: the glibc inventory (2026-11-04)

`docs/phase5-data/step5-glibc-inventory/README.md` has the method, the
data and the estimate; `tools/phase5/glibc-inventory.sh` reproduces it
from Debian's hurd-amd64 packages and sources, checked by SHA-256.

- **Reached:** 883 members of glibc and the Hurd's libraries (`hello` 880,
  a raw libports server 746, the test client 702); 84 are glibc's Mach
  layer, and the rest use 82 of its symbols directly: locks (`gsync`, under
  every lock), ports, messages, tasks and threads, memory, time and start-up.
- **Nested functions:** 11 members, 9 of them in glibc's Hurd layer and
  `htl`, rewritten for clang.
- **A narrower scope barely narrows glibc:** the libports server alone
  needs 77 of the 82 symbols (a static glibc's start-up brings the rest).
- **Estimate:** 13.5 weeks (likely 10 to 17) for the glibc part, past
  0031's 6 weeks. **The owner decided to go on with glibc** (0031's B,
  2026-11-04), recorded in 0031: Phase 5 grows by the estimate.

### Step 6: the design of the MIG backend (2026-11-04)

Implementation choices inside choice 3, 0001, 0005 and 0006, so
*(changeable)*.

- **The data the plan asked for first** (`docs/phase5-data/step6-mig/`,
  `tools/phase5/defs-rights.py` over the Hurd's 45 `.defs`): of 462
  routines, 437 pass at most 2 rights each way (279 none, 141 one, 17 two);
  6 pass 3 or 4 (`fsys_getpriv`, `startup_procinit`, `crash_dump_task`
  ...); 19 pass arrays of rights (`file_exec`, `msg_set_dtable`,
  `socket_send` ...). In `io`, `fs` and `fsys` (Phase 5's), 5 of 79 are
  over, and neither `hello` nor its client calls them.
- **MIG kept, in the tree:** GNU MIG 1.8+git20231217 as Debian ships it
  (its SHA-256 recorded; its generated parser included, so it builds with
  clang alone) in `third_party/mig/`, unchanged, plus a Joist output mode
  in new files (`-joist`), from the same parser. Built for the host with
  the rest of the tools; it reads `.defs` exactly as today.
- **A message** (0001: 4 registers, a 4 KiB IPC buffer, 2 capabilities):
  `r[0]` the routine's id (`msgh_id`) in its low 32 bits and the body's
  length in bytes in its high 32 (so both sides check lengths exactly),
  `r[1..3]` the body's first 3 words, the rest in the IPC buffer. The body is the in-line arguments in
  declaration order, each 8-byte aligned; a variable array is its count,
  then its elements; strings likewise. No Mach type descriptors: the
  interface fixes the types. A reply's `r[0]` is the return code (and the
  length).
- **Rights:** in declaration order, as the message's 2 capabilities, each
  with a mask from its MIG disposition (copy or make a send right: SEND and
  GRANT; a send-once right: SEND alone; a move: passed, then the sender's
  slot deleted). They arrive in the receiver's receive slots, which are its
  port names (glibc's business). The request port is the endpoint called;
  the reply port is the call's reply capability.
- **Over the limit** (more than 2 rights, or an array of them): not
  generated in Phase 5; both stubs return `MIG_BAD_ARGUMENTS`, and the 25
  routines are listed. They get a split or a reply-object exchange when a
  user needs them (`exec` and `proc`, Phases 7 and 8).
- **One-way routines** (0001's amendment): a `Call` the server answers at
  once with an empty reply. **Deferred replies** (`MIG_NO_REPLY`): the
  server keeps the reply capability in a reply object (`reply_save`) and
  answers later.
- **Out of line** (0006): in line while it fits the buffer; `dealloc` ones
  by move-grant (step 9); anything larger refused until step 9 measures it.
- **No system call in generated code:** user stubs call
  `joist_mig_call(port, &msg)`, which the runtime gives (`user/lib`, then
  glibc's `sysdeps/joist`); servers are `<prefix>_server(in, out)` on a
  Joist message, which libports' `ReplyRecv` loop (choice 6) calls. So the
  generated code runs on the host as it is.
- **Tests:** on the host, every routine of `io`, `fs` and `fsys` round
  trips through a fake transport: the backend also emits, behind a flag, a
  test server whose routines hash their inputs and fill their outputs from
  a seeded stream, and a client that calls every stub with seeded inputs
  and checks both sides. A libFuzzer target feeds the generated demuxer
  arbitrary messages (ids, lengths, counts, unterminated strings, missing
  capabilities): refused with `MIG_BAD_ARGUMENTS`, never a read past the
  message, under ASan and UBSan. Mutations in the backend's marshalling.

### Step 6: the MIG backend (2026-11-04)

- **Built** as *Step 6: the design* says: GNU MIG in `third_party/mig/`
  (Debian's source, unchanged, its SHA-256 in `JOIST.md`), plus `cpu.h`
  (generated once from Mach's headers) and `joistmig.c`, the Joist mode, a
  program of its own built from MIG's parser and types. Generated code
  speaks Joist's IPC through `user/lib/joist_mig.h` and makes no system
  call. One change to the design while building: `r[0]` also carries the
  body's length in bytes (its high 32 bits), since three register words
  can't tell a short body from one with trailing zeros; both sides now
  check lengths exactly.
- **`io`, `fs` and `fsys` generate and compile** with `-Werror`. 74 of
  their 79 routines are carried; the 5 the rights count found (`file_exec`,
  `file_exec_paths`, `file_get_storage_info`, `fsys_getpriv`,
  `fsys_get_children`) answer `MIG_BAD_ARGUMENTS` on both sides.
- **Tests** (`tools/mig-test/run.sh`; the Hurd's headers fetched and
  checked by SHA-256; nothing downloaded is run):
  - the round trip: every carried routine through a fake transport that
    copies as the kernel's `Call` does and moves capabilities to other
    slots; the test server hashes what it got and fills its outputs from a
    seeded stream, the client checks both, and 1 call in 13 is answered
    with an error. 74 000 calls over 1 000 seeds pass at `-O0`, `-O1` and
    `-O2`, under ASan and UBSan. Spoilt messages are refused: a word too
    many, a wrong length, an unknown id, a request or reply without its
    right, an unterminated string, an array no message can hold. Three
    by-value struct arguments (`time_value_t`, `timespec_t`) are carried
    but not hashed: after a call their padding is unspecified in C.
  - libFuzzer on the three demuxers: no crash in 6.9 million inputs.
  - 13 mutations of the generator and the runtime header, all caught.

### Step 7, part 1: glibc cross-built with clang (2026-11-04, in progress)

- **The build** (`tools/phase5/glibc-cross.sh`, on the P53: its clang 21,
  lld and bison): glibc 2.43 for `x86_64-gnu` from Linux, its Mach layer
  as it is, the Hurd's and Mach's headers from Debian (checked by SHA-256),
  MIG from `third_party/mig`. Upstream clang lacks `__float128` for the
  Hurd target, so the compiler is clang's Linux x86-64 target with the
  Hurd's predefined macros (the objects are the same), recorded for 0002.
  A build takes about 5 minutes with 2 jobs.
- **First result:** `configure` passes; `libm` builds whole (Phase 0's hang
  was on the Hurd, not in a cross-build); libc doesn't: 264 error sites, the
  root causes Phase 0 found.
- **Fixed** by six patches in `tools/phase5/glibc-patches/` (each a
  reviewable change to the Hurd layer Joist keeps, written to go upstream):
  the extern-inline guard (clang's libc has no extern inlines; the Hurd's
  and Mach's headers hid declarations behind them and made their
  out-of-line copies by turning them on: now `_LIBC` and `_FORCE_INLINES`),
  variable-sized arrays in structs, an FPU save area as an asm operand,
  and one nested function. Nothing changes for GCC.
- **Left:** 70 GCC nested functions in 31 files of the Hurd layer (and
  their knock-ons); upstream glibc still has them all. The owner chose
  context parameters (2026-11-04): each becomes an ordinary function taking
  what it used (or, for a cleanup using many locals, a local macro), and
  callbacks passed to fixed-signature helpers (`_hurd_ctty_input`,
  `use_init_port`, `_hurd_priority_which_map`, `_hurd_startup`, signal
  preemptors) get context-taking variants of those helpers. Done: all of
  them, patches 0007-0009 (0007: 7 functions in 6 files; 0008: lookups,
  signal delivery, exec; 0009: the rest). Each file compiled with clang.
- **Accepted by the owner (2026-11-05)** with 0008-0009:
  - The lookups take a `struct hurd_lookup_ctx` inside libc and ld.so
    (`sysdeps/hurd/include/hurd/lookup.h`). The exported
    `hurd_file_name_lookup` and its siblings keep their ABI: a caller's
    `use_init_port` is given a plain `operate` that reaches its operation
    through one thread-local pointer (nested lookups keep theirs).
  - `struct hurd_sigstate`'s `cancel_hook` takes `cancel_hook_arg`, a member
    added at its end (as htl's own cancel hook); no offset moves.
  - `_hurdsig_abort_rpcs`, `_hurd_priority_which_map`,
    `_hurd_ctty_input/output` and `_hurd_startup` (none exported) take a
    context; the signal handlers of `hurd_catch_signal` and `sigtimedwait`
    find theirs on the thread's preemptor list, marked by their preemptor
    functions.
  - Behaviour unchanged, upstream's quirks included.
- **Result (2026-11-05): all of glibc 2.43 builds** for `x86_64-gnu` with
  clang 21 and lld (`make` exits 0; libc.so, ld.so, libc.a, libpthread,
  libm, libanl and the rest), in about 5 minutes with 4 jobs on the P53.
  Ten patches; on the way:
  - glibc's source is now GNU's release tarball (signature checked: good,
    from the release manager), not Debian's: the same files, and `manual/`,
    which Debian drops and the Hurd's errno list is made from.
  - 0002 grew: `hurd/userlink.h`'s bodies under `_FORCE_INLINES` too, a
    declaration of `__hurd_at_flags`, and the hidden prototypes of
    `lock-intern.h`, `hurd/port.h` and `hurd/fd.h` moved before the inline
    bodies (clang ignores one after the body, so libc had no `__GI_`
    aliases and ld.so no spin locks).
  - 0010: `gai_misc.h`'s helpers `static inline`: GNU89 `extern inline` has
    no copy of its own, and only GCC inlines every call.
- **ABI** (glibc's `check-abi`, a comparison of the exported symbols with
  the lists glibc keeps for the Hurd on x86-64): every library matches,
  but for one size, `_nl_default_dirname` (the locale directory's string,
  set by `--prefix`, not by the code). libmachuser and libhurduser have no
  lists, and their step fails in glibc's awk on llvm-objdump's output.
- **Not yet run:** nothing here has run; the libraries are for a Hurd.
  Next: sysdeps/joist, step 7 proper.
