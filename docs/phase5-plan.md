# Phase 5 plan: the region capability, MIG retarget and libports

**Status: draft, awaiting the project owner.** Nothing here is built until
the owner accepts this plan and the decisions it proposes: 0030 (the region
capability) and 0031 (glibc brought forward). The owner chose their
direction on 2026-10-30: regions with several frames, and the reused Hurd
code on glibc from this phase.

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

1. **This plan, and decisions 0030 (region capability) and 0031 (glibc
   from this phase),** accepted by the owner.
2. **The region capability in the model:** the object, its calls, I12
   extended, property tests, CBMC, mutations; the carried CBMC work.
3. **The region capability in the kernel and `mm`;** measured.
4. **`mm`'s per-tree limits (0028).**
5. **The glibc inventory (0031's check):** what libports, libihash,
   libtrivfs, `trans/hello` and a test client reach in glibc without
   `sysdeps/mach`; the estimate; the owner's decision if it passes 6
   weeks. Recorded in `docs/phase5-data/`.
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

## For the owner

1. **Accept this plan** as the basis for Phase 5, or change it.
2. **Accept decisions 0030 and 0031**, or change them. The numbers in
   them are proposals: 8 extents a region, 8 regions a task, 32 tables a
   region (64 MiB), and 0031's 6-week check.

## Progress

(Empty until the plan is accepted.)
