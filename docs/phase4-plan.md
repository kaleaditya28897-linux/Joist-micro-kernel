# Phase 4 plan: memory management

Plan §11, Phase 4: *page-fault IPC, userspace memory manager, anonymous
memory, COW, shared memory grants.* **Exit: a test program can
`fork`-equivalent (via memory manager clone) and run COW correctly under
stress.**

This file turns that into concrete work, as `docs/phase3-plan.md` did for
Phase 3. It is a plan, not a decision: choices marked *(changeable)* can be
changed without a journal entry until later phases build on them. Three
choices are big enough for the journal and are proposed as decisions
**0020** (page-sized mappings), **0021** (donated page tables) and
**0022** (revoke by kill, the batched copy), in `docs/decisions/`, to be
accepted with this plan.

**Status: accepted by the project owner, 2026-10-21**, with decisions 0020,
0021 and 0022. Drafted
with a design survey by the P53's Claude Code
(`docs/phase4-data/survey.md`). What that survey says of other systems
(seL4, Fiasco.OC, NOVA, Zircon, Minix 3, Mach) is from memory and was not
checked against their sources; it is background here, not evidence. What
it says of Joist was checked against the tree.

## Inputs

What earlier phases hand to this one:

- **The kernel's memory calls** (`docs/abi.md`, *Frames and mappings*;
  `kernel/core/map.c`): frame capabilities for all free memory, given to
  the root task at boot (0013); `frame_split`; `map` of a **whole** frame
  (at most `JOIST_MAP_SPAN` = 512 pages, inside one page table) and
  `unmap`; W^X; sharing by mapping one frame in several tasks; TLB
  shootdown on SMP.
- **Fault IPC** (0009, `docs/abi.md`, *Thread control and faults*): a
  fault is a `Call` to the thread's handler (else its task's) with the kind,
  the address, the error code and a control capability for the thread; the
  reply resumes or destroys the thread. A memory manager plugs in here.
- **Thread control** (0010): registers, suspend, resume, abort.
- **The decisions that set Phase 4's shape:** 0007 (page faults to a
  userspace pager; a dead pager is a fault, not a hang), 0008 (the kernel
  does `map`, `unmap`, `protect`; `mm` owns the address-space map; the
  escape hatch is designed in Phase 4), 0011 (O(1) task creation; `mm`
  does the clone; the capability copy is explicit and batched;
  `posix_spawn` native), 0013 (the root task partitions memory), 0003 (no
  allocation after boot; revoke blocks rather than defers), 0006 (Phase 5's
  move-grant, which Phase 4 must not make impossible).
- **Carried forward** (`docs/phase3-exit.md`): revoke and 0011's batched
  copy; page-fault IPC to a memory manager; donated memory (seL4-style
  untyped), reconsidered with it; guard pages under the double-fault, NMI
  and machine-check stacks; `run.c`'s broken-kernel tests not kept as
  mutations; gaps in fault-injection coverage.
- **A promise to keep:** the plan (§7.3) and `docs/client-recovery.md` say
  that what a file-backed `mmap` does when its pager dies is decided in
  Phase 4, not Phase 9, though file-backed memory itself is Phase 6.
- **The tree's limits, found while planning** (they shape the choices):
  - The kernel's tables are global, fixed and scanned whole: 1 024 objects
    and 512 mappings in the kernel build (`kernel/Makefile`, `KSIZES`), 16
    address spaces, 32 threads, 8 192 capability slots per space from a pool
    of 64 leaves. `map` and `unmap` scan the mapping table; making a frame
    scans the object table.
  - A mapping is a whole frame, and `frame_split` refuses a frame that is
    mapped or shared, so a page can't be made private to one task after it
    has been shared: there is no page-sized COW.
  - Page tables come from a pool reserved at boot for the worst case of the
    mapping table (1 560 frames), charged as one quota unit per mapping.
  - A capability is `{object, type, rights, badge}`, with no record of what
    it was derived from: nothing to revoke along.
  - `map` and `unmap` on the caller's own space (`JOIST_SLOT_SELF`) need no
    task capability, and `unmap` needs no frame capability: any task can
    remove what `mm` mapped into it.

## Scope

| In Phase 4 | Not until later |
|---|---|
| Mapping part of a frame; page-sized mappings with the page tables as the record (0020) | huge pages (2 MiB mappings) |
| `protect`, `unmap_range`, a conditional `map`, `map_query` | — |
| Page tables paid for by memory the memory manager donates (0021) | swap, the default pager (Phase 6 and later) |
| Revoke by killing the object; the batched capability copy for `fork` (0022) | revoke of descendants (a derivation tree), if RS needs it |
| The fault message completed (pc, an access type); then a fault reply that maps, once measured (0007) | signals over fault IPC (Phase 7) |
| `servers/mm`: anonymous memory, demand-zero, COW clone, shared mappings between tasks | file-backed memory and the paging protocol (Phase 6) |
| What a file-backed mapping does when its pager dies: decided and written down | its implementation (Phase 6) |
| The escape hatch of 0008: designed, then built after the exit test | measured against glibc's `malloc` (before Phase 7) |
| `fork` of a test program by `mm`, and `posix_spawn`'s kernel side (an empty task) | glibc's `fork` and `posix_spawn` (Phase 7) |
| The `SELF` gap closed | — |
| Capacities raised for fork trees, still compile-time constants (choice 10) | — (not sized from boot memory: the owner, 2026-10-21) |
| Guard pages under the double-fault, NMI and machine-check stacks | — |
| `mm` restartable by design (its state readable back from the kernel) | `mm` actually restarted (class C, Phase 9) |
| — | moving pages between tasks (move-grant, 0006, Phase 5) |

## Constraints carried in

- **§5.1:** no recursion, no VLAs, no allocation after boot except from
  memory a task donates, bounded work per system call, user memory only
  through `copy_from_user`/`copy_to_user`, the 15k-line tripwire (the
  kernel is 7 879 lines at Phase 3's exit, headers included).
- **0003 and 0016:** one kernel lock, interrupts off, bounded preemption
  points; nothing user-sized under the lock. A call over pages is bounded
  by one page table (512 pages) and does at most one shootdown.
- **0004:** every object paid for from a task's quota.
- **TLA+'s rule:** a dying space accepts no new capabilities. Phase 4 adds:
  and no new mappings.
- **§9.1:** `mm` reads and writes all user memory and is in the trusted
  base; its authority is the frames it is given and control of the tasks
  it serves, nothing ambient.
- **Model first.** Every kernel change goes into the model and its tests
  (property tests, mutation, fuzzing, CBMC, TLA+ where it applies) before
  the kernel relies on it. `mm`'s COW logic is host-testable too, against
  the model.

## Choices this plan makes

1. **Page-sized mappings: map part of a frame, and the page tables are
   the record (decision 0020, proposed).**
   - `map(frame, page offset, n, task, va, perms)`, with the run inside one
     page table. The global mapping table goes. Each page-table page gets a
     **shadow page** holding, per entry, the frame it names and the rights
     the frame capability allowed. Each frame keeps a **map count**.
   - So `mm` holds a few large frames (hundreds of frame objects, not
     hundreds of thousands), allocates pages from them itself and keeps
     the COW reference counts. The kernel keeps only what makes the page
     tables checkable: which frame each entry names, and how many name it.
   - Deleting a frame's last capability is O(1) when its map count is 0.
     Otherwise the frame is **dying**, and a sweep of the spaces' shadow
     pages joins the destruction work list (0016's preemption points),
     one page table per step. If that sweep proves too slow, a reverse
     map threaded through the shadow pages is the fallback (twice the
     shadow memory).
   - I12 becomes: every present user entry names a page inside a live (or
     dying) frame; the shadow agrees with the entry; the map counts add up
     to the present entries; W^X; user half only.
   - Rejected: one frame per page (a 1 GiB space would need 262 144 frame
     objects, capabilities and mappings, against tables of 1 024 and 512),
     and a capability per mapping as seL4 does (Joist's slot limit and its
     object record make it the most expensive).
2. **The kernel calls** *(changeable in detail)*, each over at most one
   page table, with one shootdown:
   - `map`, extended: a page offset and a count; and a **conditional
     replace** (`REPLACE`, with the entry each page is expected to hold),
     which fails with no effect if any page differs. It lets `mm` resolve a
     fault without locks of its own around the kernel call, and closes the
     Dirty COW class of race (a discard racing a write fault).
   - `unmap_range` (holes allowed), replacing `unmap`'s "a mapping starts
     here".
   - `protect` over a range, up to each page's rights ceiling, W^X checked.
     An upgrade needs no shootdown (a stale read-only entry only refaults).
   - `map_query`: per page, present, permissions, page offset, and the
     frame only if the caller holds that frame with CONTROL. Never a
     physical address. Needed to check `mm` against the kernel (0008) and
     for `mm`'s restart (class C).
   - **No kernel copy.** `mm` keeps the memory it manages mapped in its
     own space (read-write, never executable) and copies and zeroes pages
     with `memcpy`. A kernel copy would hold the one lock for microseconds
     per page; mapping both pages into `mm` for each copy would cost a
     shootdown per unmap.
   - **The `SELF` gap closed:** mapping into or unmapping from one's own
     space needs a capability to one's own task with CONTROL, as for any
     other task. `mm`'s clients don't get one.
3. **Who pays for page tables: memory the memory manager donates
   (decision 0021, proposed).** `space_donate(task, frame)` turns a frame
   into page-table and shadow pages for that space; the kernel holds it
   until the space dies, then it is the donor's frame again. `mm` donates
   when it creates a task and before it maps beyond what was donated (it
   knows its own layout), so `map` without enough donated pages is
   `E_NOMEM`, never a kernel allocation. Memory is conserved by
   construction (I3): the pages never leave a frame object. The boot pool
   shrinks to the root task's tables and each space's top-level table.
   Objects stay on quota units as today (0004). **Fallback**, if donation
   proves too much for Phase 4: one quota unit per page-table page, the
   pool a compile-time constant like the other capacities (choice 10); the
   decision would say which was built.
4. **Revoke and the batched copy (decision 0022, proposed).**
   - **Revoke by killing the object**, as an endpoint dies today: every
     capability to it then fails `E_GONE` at once, O(1); the slots stay
     occupied until their holders delete them. It needs a kill right, so
     copies can be handed out without it; a frame lent to a space can't be
     killed. Built for the objects that need it in Phase 4: the region
     capability (choice 7) and frames.
   - **No derivation tree** (seL4's) in Phase 4: Mach has no revoke of
     descendants and the Hurd does not rely on one. Kept as a recorded
     option (8 bytes more per slot) for when RS must revoke what it
     delegated through a server.
   - **`cap_copy_batch(task, first, count ≤ 32, rights mask)`** for
     `fork`: copies each capability in `[first, first + count)` of the
     caller's space to **the same slot** of the child (the Hurd's `fork`
     keeps port names, `docs/client-recovery.md` §4), masked; skips those
     without GRANT, and receive rights unless asked; stops at an occupied
     slot or a missing leaf with a cursor; `E_DEAD` for a dying child.
     Never allocates (a derive only counts a reference). 32 slots is the
     destruction step's size. The batch is atomic only per call; Hurd
     glibc stops the parent's other threads for the whole copy (checked
     2026-10-27, *Risks*).
5. **The fault path** (0007, 0009). First, as it is: one fault IPC to
   `mm` per fault, then `mm`'s kernel calls, then the reply. The message
   gains the **faulting pc** and an **access type** (read, write or
   execute; present or not) independent of the x86 error code. A handler
   may ask for **no thread capability on page faults** (the badge names the
   task), saving a `cap_delete` per fault. **Measured, then fused:** a
   reply that carries one map action (0007's "reply carrying a frame
   capability"), if the measurement shows the extra kernel entry matters.
   Read faults and demand-zero map up to 16 pages around the fault; write
   faults one page. *(changeable)*
6. **`servers/mm`** (C, Joist's coding standard; §12's place).
   - The root task gives `mm` the memory it is to manage and control
     capabilities for the tasks it starts; `mm` is their fault handler.
   - **`mm` never hands out a frame capability** to a task it serves
     (0022's amendment, 2026-10-23): a frame kill can't take back the
     pages while a holder keeps a dead copy. Tasks get authority over
     memory only as regions (choice 7).
   - It keeps, per task, a region map; per page it manages, a reference
     count and its owner. Anonymous memory is demand-zero.
   - **Clone** (0011): the kernel makes an empty task in O(1); `mm`
     walks its own records, **downgrades the parent's writable pages
     first** (`protect` with shootdown), then maps them read-only into the
     child, then the capabilities are copied in batches. A write fault
     copies the page if it is shared, or makes it writable if it is now the
     last reference.
   - **Shared memory between tasks:** `mm` maps the same pages into both
     (`mm_share`); no new kernel object. Pages shared this way are never
     COW-shared.
   - Its logic sits in a library built for the host as well, so it is
     tested against the kernel model (*Testing*).
   - Restartable by design: everything it needs to rebuild is readable
     from the kernel (`map_query`) and the capabilities it holds.
     Restarting it is Phase 9's.
7. **The escape hatch** (0008). A **region capability**: a range of one
   task's addresses, a backing frame, a rights ceiling. Its holder may
   map, unmap and protect pages of that frame inside the range, and
   nothing else; `mm` keeps the frame and can kill the region (choice 4).
   Designed now, so the calls above are shaped for it (and so it can serve
   as Phase 5's receive window). Built after the exit test, and measured
   against plain RPCs to `mm` before Phase 7, as 0008 asks. *(changeable)*
8. **A file-backed mapping whose pager dies.** Proposed: as 0007 says, the
   faulting access becomes a fault to the task's own handler (`SIGBUS`
   once glibc exists); `mm` does not keep a file's pages to hide the
   pager's death. Pages already mapped stay readable until `mm` unmaps
   them. Written into `docs/client-recovery.md` with this plan; built in
   Phase 6.
9. **Moves and COW, for Phase 5** (0006): only `mm` knows which pages are
   COW-shared, so a kernel move of pages could hand a client a page a fork
   child still sees. Rule for Phase 5: the kernel moves only pages in
   region ranges, which `mm` never COW-shares (it copies them at fork).
10. **Capacities** (the owner, 2026-10-21): raised for fork trees to 64
    address spaces, 128 threads and 2 048 objects, with capability leaves
    raised to match. They stay **compile-time constants**: they are not
    sized from boot memory. The tables are still scanned in places (making
    an object, finding a free space); each scan's bound is recorded, and an
    O(1) free list replaces any scan on a path Phase 4 makes hot.

## Testing

| What | How |
|---|---|
| The new kernel calls and I12 as revised | the model first: unit and property tests under every sanitizer, whole states compared after every step, including "a failed call changes nothing" for partial ranges and for the conditional `map`; CBMC on `map`, `unmap_range`, `protect`, `space_donate`; mutations; the fuzzer |
| Revoke and the batched copy | property tests against a shadow; TLA+'s dying-space rule extended to mappings and to `cap_copy_batch` |
| COW, refined | **an abstract machine** in which every task owns an array of page values and `fork` deep-copies it, against the concrete one (the kernel model, `mm`'s library and the fault calls) on random sequences of fork, read, write, unmap, protect, share, exit and kill. Every read must equal the abstract value; `mm`'s view must cover the kernel's mappings (0008). Seeded, replayable, mutated |
| Stale TLB entries | TLA+: per-CPU TLBs as stale caches, two threads of a task on two CPUs, `mm` replacing a page. Removing the shootdown, or mapping the child before the parent is downgraded, must violate the property |
| In QEMU and on the P53 | `user/cowstress`: fork trees 4-6 deep, every page tagged (task, generation, address, checksum), written at random and verified after every step and at exit, by writers on 1, 2 and 4 CPUs; children killed mid-fault and mid-fork (the `faultinj` pattern); the kernel's counters (frames, page-table and shadow pages, map counts) level after every round; also on the checking kernel |
| Mutations (model and kernel) | among others: no shootdown on a replace; the child mapped before the parent's downgrade; a reference count off by one; a write upgrade with the page still shared; a map count not decremented; a sweep that skips a page table; `cap_copy_batch` widening rights; a replace that ignores what it expected; a discard racing a write fault (Dirty COW's shape) |
| Measured, recorded | fault round trip (read, write-copy, write-upgrade), `fork` of 1, 16 and 256 MiB, `protect` over 512 pages; to `docs/phase4-data/` as Phase 3 did. Not gated at first; a regression gate once stable |
| **Exit: `fork` and COW correct under stress** | *Correct under stress* means: `cowstress` passes N rounds (N fixed with the owner before the exit run) on 1, 2 and 4 CPUs, under KVM on the P53 and on the checking kernel, with every read verified and every counter level after every round; the refinement property test passes with M seeds; every listed mutation is caught |
| Phase 3's suites | all of them, unchanged, in every full run |

## Order of work

1. ~~**This plan, decisions 0020, 0021 and 0022**, accepted by the owner.~~
   Done 2026-10-21.
2. **The model:** part-of-a-frame mappings, shadow and map counts,
   `unmap_range`, `protect`, the conditional `map`, `map_query`, the dying
   frame's sweep; I12 as revised. Property tests, CBMC, mutations.
3. **The model:** donated page-table memory (0021); revoke by kill and
   `cap_copy_batch` (0022); TLA+ extended.
4. **The kernel:** the page tables as the record, shadow pages, donated
   pages, the shootdown per call; the `SELF` gap closed; capacities raised.
   Every Phase 3 self-test still passes; new self-tests for the calls.
5. **The fault path:** pc and access type in the message, no thread
   capability on request. Measured.
6. **`mm`:** the library and its refinement test on the host, then the
   server: anonymous memory, demand-zero, clone, shared mappings. The root
   task starts it and hands it memory.
7. **Exit:** `cowstress`, the measurements, the fused fault reply if
   measured worthwhile, the file-backed rule written down, the guard pages,
   the exit record.
8. **After the exit test:** the region capability (choice 7), built and
   measured. If it lands after the exit record, the record says so.

## Risks

- **The one lock and long calls.** `protect` or `map` over 512 pages,
  with a shootdown, under the one lock, is the longest call so far.
  Measured at step 4; if it is too long, the bound drops below one page
  table.
- **Line count.** Shadow pages, donation, revoke, the batched copy and the
  calls may add 1 500-2 500 lines: about 10 000 of the 15 000-line
  tripwire. Counted at every step.
- **`mm` is in the trusted base** and is the first real server. Its
  authority is kept to the frames and task capabilities it is given (0013);
  the review of it is part of the exit.
- **Hurd glibc's `fork`** stops the parent's other threads for the copy
  (checked 2026-10-27 in glibc's `sysdeps/mach/hurd/_Fork.c`, the
  current tree: `__proc_dostop` before `__mach_port_names` and the
  copy loop, `resume_threads` only after the ports are in the child), so
  `cap_copy_batch` needs no stop of its own. Joist's `proc` will need its
  `dostop` (suspending every thread of a task but one, through thread
  capabilities, 0010); that is Phase 7's.
- **Donation is new state in the kernel** (a frame lent to a space). If it
  proves too much, the fallback in choice 3 is ready, and the decision
  records which was built.
- **Bare metal** is still not available; every real-CPU figure is under
  KVM on the P53.

## Progress

### Step 2: the design of the model's page tables (2026-10-21)

How 0020 and 0021 go into `kernel/core/`. These are implementation
choices inside the accepted decisions, so they are *(changeable)*.

- **0020 and 0021 go in together.** Every root program maps memory
  (`user/lib/loader.h`, `testroot.h`, `faultroot`, `faultinj`,
  `growroot`), several into their own space. Building 0020 on the old boot
  pool first would be replaced by donation at once, so the core, the
  kernel's hooks and those programs change in one part. The kernel keeps
  building and passing at the end of each part.
- **The core keeps a shadow tree that mirrors the hardware's levels.** It
  must know which tables a run needs, to fail with `E_NOMEM` before
  changing anything, and which become empty.
  - The top level (the user half's 256 entries) is in the core's state,
    per space: a compile-time size, like the other capacities.
  - Each level below is a node of one page in a donated frame, paired with
    the hardware table it mirrors (two pages per table). An interior entry
    names both pages of its child.
  - A last-level entry is the 0020 record: the frame, the page offset in
    it, the permissions and the rights ceiling.
- **The core reaches donated pages through a hook** that turns a page
  number into a pointer: the kernel's direct map, or a simulated physical
  memory in the model, as `kernel/mem/vm.c`'s tests already use.
  `vm.c` keeps writing x86 entries, but into tables the core gives it
  (`table_new`, `table_gone` hooks); it no longer takes or frees pages for
  the user half itself.
- **Donated pages** (0021): a space keeps a list of the frames lent to it,
  each with a high-water mark, and a free list of pages given back by
  emptied tables, threaded through the free pages. Taking a page is O(1),
  and so is `space_donate`, whatever the frame's size.
- **Emptiness is found by scanning**: when `unmap_range` clears entries,
  the leaf it touched and the levels above are checked (at most 3 × 512
  entries); no per-table counts.
- **Two kinds of sweep go on the destruction work list** with a cursor
  over the shadow tree, one leaf table per step (0016's preemption
  points): a dying space's tables (replacing `joist__space_unmap_all`) and
  a frame that dies while still mapped (replacing `joist__frame_freed`'s
  scan).
- **0022's calls** (`obj_kill`, `cap_copy_batch`) are built in parallel by
  the P53's Claude Code in its own copy, and reviewed before they are
  applied.

### Step 2, part 1: page-sized mappings and lent page tables (2026-10-21)

0020 and 0021 in the core, the kernel and the user programs together, as
the design above says.

- **The core** (`kernel/core/map.c`, rewritten): the shadow tree per space,
  pages taken from lent frames or from a free list of pages given back,
  `space_donate`, `map` of any run of a frame, `map_replace`,
  `unmap_range`, `protect`, and the two sweeps on the work list (a dying
  space's tables, frames dying while mapped). `joist_boot_self` gives the
  root its capability to itself. The global mapping table and its quota
  charge are gone. A frame whose last capability goes while it is mapped
  or lent is kept, dying, by splitting `obj_unref` (`cspace.c`) in two.
- **Found while building it:**
  - With a capability to itself, the root could be destroyed by
    `task_destroy`: the whole-model property test found it in its first
    run. `task_destroy` now refuses the root's own capability
    (`E_RIGHTS`), and deleting it only forgets it.
  - The property test's teardown expected every unit back in the root.
    Frames lent to the root's own space stay until it dies, which it never
    does (0021 has no call to take lent pages back from a live space), so
    they are now the one thing teardown may leave, dying and charged to
    the root.
- **The kernel:** `vm.c` installs and removes tables the core places
  (`vm_table_set`, `vm_table_clear`), replaces (`vm_remap`), protects
  (`vm_protect`) and forgets a space's lower half without freeing it
  (`vm_release`); it no longer takes or frees a user table itself. `run.c`
  flushes the TLB on every change that can leave a stale entry (a replace,
  an unmap, a protect, a table that goes). The page-table pool shrinks to
  the spaces' top-level tables. At boot the root gets its own capability
  (slot 5) and a frame of 256 pages lent to its space (slot 6), before its
  image is mapped (`run.h`).
- **System calls 41-45** (`docs/abi.md`, *Frames and mappings*):
  `map` with an offset and a count, `unmap_range`, `map_replace`
  (arguments 7 and 8 in `R12`, `R13`), `protect`, `space_donate`.
  `debug_counters` counts page entries present, and lent pages used by
  tables (a new last field).
- **User programs:** the loader lends each task it makes 32 pages for its
  tables (`ld_tables`), and maps into the root through `J_ROOT_SELF`;
  `growroot` now exhausts a task's lent pages (a map 2 MiB apart each
  time) where it exhausted the mapping table, and it and `faultinj` check
  the new counter stays level.
- **Tests** (`model/test/test_map.c`, 16 tests): lending and its refusals,
  a run refused for one page too few and fitting with exactly enough, a
  frame refused when its first or its last page can't be reached,
  the tables a run takes, runs of a frame, refusals that change nothing,
  the conditional replace, `unmap_range` with holes giving tables back,
  `protect` up to the ceiling, a dying frame swept a table per step,
  task destruction returning its lent frame, a lent frame dying with its
  space, mapping into one's own space, and the checker seeing corrupt
  entries. The whole-model executor keeps a simulated set of hardware
  tables and a simulated physical memory, and checks the hooks against
  the core's tree after every step; it maps, replaces, unmaps, protects
  and lends at random. New coverage floors: steps with page tables and
  with a frame dying while mapped; successful maps, unmaps, protects and
  lendings.
- **Mutations:** 13 of Phase 3's mapping mutations aimed at code that is
  gone. Twelve were retargeted to the same rule in the new code;
  `map-uncharged` went with the rule (a mapping no longer costs a quota
  unit). 20 new ones were added: 32 in all (`map-`, `lend-`, `root-self-`,
  `hook-`). The first run found three that didn't build (`if (0)` left a
  function or variable unused, which `-Werror` refuses; they now keep it
  and add `&& 0`) and two that survived, each a boundary no test reached:
  lending one page too few for a run's tables (`map-pages-needed-short`),
  and a frame unreachable at its first page only (`lend-unreachable`).
  Two unit tests above now reach them.
- **Also found by the first full run:**
  - I9's closed-form bounds (`model/test/exec.h`) had no term for page
    tables, so the `small` build's bound (776) was below what one
    `unmap_range` may do (933: a table's entries, then the three tables
    above it scanned for emptiness). Each bound now has a page-table
    term: a run's work for a simple operation, and up to three sweeps of
    every space's tables, as many as the lent memory holds, for one that
    cascades.
  - `faultinj` subtracted two `ld_mark()`s to count a round's pages, which
    the loader's new mark (pages and runs of table pages together) broke.
    It now counts each (`ld_pages_since`, `ld_runs_since`), and warms a
    run of table pages for each peer as it warms pages (`ld_warm_runs`),
    so the first round can't read as growth.
  - `smproot`'s destroy stage (20 rounds of a task per other CPU, never
    given back) ran its 4-CPU variants out of memory once each task took
    32 lent pages: 60 tasks, 1 920 pages of a 2 048-page pool. Its work
    pages stay mapped in the root, but the tables' runs come back when
    the tasks die, so it now gives the runs back each round
    (`ld_release_runs`).
  - The bench gate's do-nothing call read 632 cycles (limit 620; 380 in
    0018's check). That run overlapped the P53's Claude Code's own kernel
    check on the same build VM; rerun alone, see below.
- **Verified** (2026-10-21 and -23, on the build VM, each run with
  nothing else on it):
  - `make -C model check`: 182 tests under plain, asan, ubsan, msan, tsan
    and small, no failure; the whole-model property test with the floors
    above.
  - Mutations `map-`, `lend-`, `root-self-`, `hook-`: all 33 caught.
  - `make -C kernel check`: every host test, freestanding build,
    clang-tidy and scan-build clean, BIOS and UEFI boots, and all 71
    self-tests, among them the 4-CPU SMP tests, the memory-growth test and
    the seven fault-injection runs. The bench gate: a do-nothing call 412
    cycles, two spaces 1 568, one space 1 104 (baselines 496, 1 548,
    1 092).
  - Not run for this part: CBMC (the mapping calls are not among its
    operations; adding them is step 2's next part).

### Step 2, part 2: revoke by kill and the batched copy (0022) (2026-10-23)

Built by the P53's Claude Code in its own copy, against `5770fbe`, while
part 1 was being built (its report: `docs/phase4-data/0022-report.md`),
then merged and adapted to 0020 and 0021 here.

- **What it built:** `joist_obj_kill` with the kill right (`JOIST_R_KILL`,
  the SEND_ONCE bit, for frames; written so regions can be added), boot
  frames with the right, and every use of a killed frame `E_GONE`;
  `joist_cap_copy_batch` (at most 32 slots, the same slot numbers, rights
  masked, a skipped bitmap, stopping with a cursor); system calls 28 and
  29; `user/revokeroot`, run emulated, on 2 real CPUs and on the checking
  kernel; unit tests (`test_kill_copy.c`), a property test against a
  shadow of the whole state (`test_props_kill_copy.c`), the executor's
  two operations, 21 mutations, and CBMC operations 9 and 10, both
  verified.
- **Adapted to 0020 and 0021:**
  - A killed frame is unmapped by 0020's sweep, as a dying one is: the
    sweep takes frames dying *or* killed (`condemned`), each counted once
    in `frames_dying` (a killed frame whose last capability then goes is
    not counted again), and `obj_kill` runs that sweep, as far as
    preemption allows, but no space's destruction already pending (it
    began none; `joist_continue` takes those). Its unit tests check it a
    table per step, preempted.
  - A frame lent to a space can't be killed (`E_STATE`): its pages are the
    space's tables. A killed frame can't be lent (`E_GONE`, as every use).
  - The property test no longer maps: its shadow mirrored Phase 3's
    mapping table, and what a kill does to mappings is covered by the
    executor (kills among real mappings, the simulated tables and every
    invariant after each step), the unit tests and `revokeroot`.
  - The executor counts `obj_kill` among the operations that may cascade
    (I9: its sweep is a cascade's work).
  - CBMC's default operations now include 9 and 10.
- **0022 amended** (the owner, 2026-10-23): the report found that a kill
  takes back authority but not memory, since a killed frame's pages stay
  its own until the last holder deletes its copy. `mm` never hands out a
  frame capability; it gives regions (choice 6 and 7 above). The frame
  kill stays for frames lent to trusted servers.
- **A kernel bug in part 1, found while merging:** `run.c` went on with
  pending destruction, and kept the fast path off, only while
  `K.npending` was nonzero, so a frame sweep (`frames_dying`) cut short by
  a pending interrupt waited for some later destruction or an explicit
  `continue`. Both now ask `joist_destruction_pending` (`core.h`): a
  dying space, or a frame dying or killed while mapped.
- **Open, from the report:** 0022 says a batch is "the size of one
  destruction step"; each copy also costs up to
  `JOIST_MAX_BADGE_ACCTS` steps (the account lookup), so a batch's bound is
  32 × (1 + accounts). And KILL shares its bit with SEND_ONCE, so a mask
  that strips it from every slot would strip SEND_ONCE from endpoints too;
  harmless while SEND_ONCE means nothing.
- **Found in verification** (2026-10-23):
  - The frame sweep went round for ever when `frames_dying` was wrong
    (five mutants hung rather than failing). A round that clears nothing,
    with no frame begun dying during it, now stops the sweep, and the
    checker reports the count (I12). A correct kernel never takes it.
  - A killed frame whose last capability went mid-sweep was not tested
    (a surviving mutant); a unit test now is.
  - `obj_kill` ran every pending space's destruction, which made CBMC's
    op 9 a hundred times larger; it now runs the frames' sweep only.
  - CBMC ran out of memory on the page tables' state: its states now
    have none, and `JOIST_PT_TOP` can be set smaller for it. Proving the
    mapping calls with CBMC is step 2's next part.
- **Verified** (on the build VM, nothing else on it; this commit's code):
  - `make -C model check`: 200 tests under plain, asan, ubsan, msan, tsan
    and small, no failure.
  - Mutations `map-`, `lend-`, `root-self-`, `hook-`, `kill-`, `copy-`,
    `batch-` and `task-no-drain`: all 55 caught.
  - CBMC: operations 0, 1, 2, 4, 5, 7, 8, 9 and 10 verified.
  - `make -C kernel check`: every host test, freestanding build,
    clang-tidy and scan-build clean, BIOS and UEFI boots, and the
    self-tests, among them `revokeroot` emulated, on 2 real CPUs and on
    the checking kernel. The bench gate: 384, 1 576 and 1 116 cycles
    (baselines 496, 1 548, 1 092).
  - Two emulated 4-CPU timing tests failed once each, in different runs
    (`smp4-cascade`: "the machine stalled too often to measure";
    `smp4-smp`: "spinners on separate CPUs didn't run at the same time"),
    and passed three times out of three alone. Recorded as load, to watch:
    a third would be worth a look at what else the P53 runs.

### Step 2, part 3: CBMC for the mapping calls (2026-10-24)

The Testing table promises CBMC on `map`, `unmap_range`, `protect` and
`space_donate`; part 2 left them unproved because the page tables'
nodes (pages of 512 entries reached through a hook) were beyond CBMC.
The owner chose to make the tables' shape a parameter and simulate the
pages in the harness (2026-10-23).

- **The shape a parameter** (`core.h`): `JOIST_PT_BITS` index bits per
  level (9) and `JOIST_PT_TOP` top-level entries (256), with
  `JOIST_PT_SHIFT(level)`; `JOIST_PT_ENTRIES`, `JOIST_MAP_SPAN` and
  `JOIST_USER_PAGES` follow from them. `map.c` and the model's tests use
  them instead of 9, 18 and 27. `run.c` requires the hardware's (9 and
  256), and `map.c` three levels.
- **The harness** (`model/cbmc/harness_cspace.c`, `cbmc/run.sh`):
  operations 11-15 are `map`, `map_replace`, `unmap_range`, `protect`
  and `space_donate`, from any state the invariant check accepts with
  page tables, lent pages and mappings, in nodes of 2 entries under one
  top-level entry. Their pages are simulated (6, behind `hooks.page`),
  in one space; lent frames lie within them, as `space_donate` checks.
  Every table page is held once: assumed of the state before and
  asserted of the one after. The other operations start from states
  without tables, with the page hook NULL, so a table page reached there
  would fail the proof. Three loops get their own bounds, found by their
  source text (CBMC numbers loops by position).
- **Found by CBMC:**
  - The map check followed a lent frame's water mark past its pages
    when the mark was damaged (it reports the damage, and went on).
    `lent_holding` now counts only below a sound mark.
  - I12's check never asked that a table page be held once (as two
    nodes, or a node and a free page): a state that breaks it passes,
    and clearing an entry in one node then corrupts the other. The
    kernel can't reach such a state (`take_page` hands each lent page out
    once), and a check of it in the kernel takes storage or time growing
    with the tables, so it is the harness's (above).
- **Freeing a frame is its own** (`joist__obj_clear`, the end of
  `joist__obj_free`): nothing waits on a frame or is notified of it, and
  it has no lifecycle, so the rest did nothing. Through it the proof of
  `unmap_range` was 16 times larger.
- **Mutations:** `map-sweep-skips-tables`, `map-sweep-skips-top`
  (the sweep's strides) and `map-frame-free-skipped`.
- **`make -C model cbmc`** now runs all fourteen operations, one at a
  time: they need 3 to 6 GB each, and two can't share the build VM.
- **Verified** (on the build VM, nothing else on it; the mapping
  calls, the model check, the mutations and the kernel check run by
  the P53's Claude Code, `/srv/joist/handoff/p4-cbmcmap/findings.md`):
  - CBMC, operations 11-15: `map` 2 795 s, `map_replace` 1 699 s,
    `unmap_range` 3 637 s, `protect` 2 072 s, `space_donate` 1 367 s;
    each 0 of about 10 380 properties failed; peaks 3.4 to 5.1 GB.
  - CBMC, operations 0, 1, 2, 4, 5, 7, 8, 9 and 10, again (their states'
    page hook now NULL): all verified, 93 to 737 s, each 0 of about
    10 290 properties failed. Run with the page hook set, as the mapping
    calls have it, `obj_kill` ran out of memory (7.9 GB) where NULL takes
    130 s; the P53's Claude Code found it and the remedy.
  - `make -C model check`: 200 tests under plain, asan, ubsan, msan, tsan
    and small, no failure.
  - Mutations `map-`, `lend-`, `root-self-`, `hook-`, `kill-`, `copy-`,
    `batch-` and `task-no-drain`: all 58 caught.
  - `make -C kernel check`: every host test, freestanding build,
    clang-tidy and scan-build clean, BIOS and UEFI boots, all 74
    self-tests (no timing test failed this time). The bench gate: 384,
    1 580 and 1 116 cycles (baselines 496, 1 548, 1 092).
- **Open:**
  - CBMC for `obj_kill` and `cap_delete` from states with mapped frames
    (a condemned frame's sweep) is not done; the sweep is covered by the
    property tests and the mutations.
  - `map-sweep-skips-top` is caught by the general I12 count, at the
    same seed and step as `map-destroy-keeps`, not by a test aimed at
    the top-level stride.

### Step 2, part 4: the capacities, and objects found in O(1) (2026-10-26)

- **Raised** (choice 10): the kernel's build (`kernel/Makefile`, `KSIZES`)
  now has 64 address spaces, 128 threads, 2 048 objects and 256
  capability leaves (4 a space, as the model's defaults have), from 16,
  32, 1 024 and 64. Still compile-time constants. The self-tests' large
  build (the cascades') keeps its 8 192 objects and takes the new
  threads and leaves. The kernel's writable tables come to 0.6 MB.
- **The scans** (choice 10 asks each scan's bound recorded, and an O(1)
  free list where Phase 4 makes one hot). Full-table scans outside the
  checks:
  - Finding a free entry: objects in `joist_obj_create`, `new_frame`,
    `free_object` (task and thread objects) and `joist_sc_create`, up to
    2 048 steps; threads in `joist_thread_create` and
    `joist_thread_spawn`, up to 128; spaces in `joist_cspace_create` and
    `joist_task_create`, up to 64; leaves in `joist_cspace_grow`, up to
    256. Every object a `fork` or `mm` makes goes through the first, so
    it is the one Phase 4 makes hot.
  - Destruction: `space_finish`, `joist__space_tables_gone`,
    `joist__object_freed` (a notification's references) and
    `joist__threads_destroy_owned`, once per destruction or per step of
    one.
  - The scheduler: `joist_tick`, `joist_sched_next_event`, `best_for`
    and `choice`, over the 128 threads; the bench gate did not move.
- **Objects found in O(1)** (`joist__obj_peek`, `joist__obj_take`): every
  object at or above a high-water mark has never been used; below it,
  the free ones form a list, the last freed first, through links kept
  apart from the objects (so a free one stays all zero, and a state of
  all zeros is an empty table). The five scans are gone. The check adds:
  nothing at or above the mark is in use, and the list names each free
  object below it once and ends. Mutations: `objfree-not-pushed`,
  `-not-popped`, `-mark-not-raised`, `-link-kept`.
- **The checks at these sizes:** the checking kernel runs every check
  after every call. At these sizes three 4-CPU self-tests on it timed
  out or made too few calls. Measured (a host program at the kernel's
  sizes, `gprof`): one check took 6.4 ms, 1.6 ms at the old sizes, 71 %
  of it in the badge accounts' check (I4), which read every slot of
  every space (64 × 8 192, absent leaves too) once, then again for each
  live account. Others were quadratic too: frames' overlaps (each frame
  against every object), reply holders (every thread and object for
  each waiting caller), dead task and thread objects (every space and
  thread for each). Now:
  - the slots are walked leaf by leaf, skipping absent leaves, and each
    account's senders are counted in that walk;
  - frames are sorted by their first pages (a heap sort) and swept;
  - holders are counted for all threads in one pass;
  - a dead object still named is found from the naming side, which was
    already checked.

  One check now takes 0.3 ms (21 times less). Mutation
  `map-check-frames-unsorted`; unit test of an overlap between frames
  far apart. The accounts' check keeps its tables static, as the others
  do (8 KB at these sizes: too large for a kernel stack).
- **Found on the way:**
  - The first run of the kernel check at these sizes tested the old
    ones: `make` didn't rebuild objects when `KSIZES` changed, and the
    build directory was reused. The image's flags are now stamped like
    its sources (`kflags.list`), so a change rebuilds every object.
  - `growroot` stopped at 1 100 objects, "past any table's size", which
    2 048 no longer is: now 2 200.
- **Verified** (on the build VM, alone, kernel built from nothing; CBMC
  run by the P53's Claude Code, `/srv/joist/handoff/p4-cbmc14/findings.md`):
  - `make -C model check`: every test under plain, asan, ubsan, msan, tsan
    and small, no failure.
  - Mutations, the whole list: all 224 caught.
  - `make -C kernel check`: every host test, freestanding build,
    clang-tidy and scan-build clean, BIOS and UEFI boots, all 74
    self-tests, the 4-CPU ones on the checking kernel among them. The
    bench gate: 376, 1 584 and 1 116 cycles (baselines 496, 1 548,
    1 092).
  - CBMC, all 14 operations: verified, 0 of about 10 450 (0-10) and
    10 550 (11-15) properties failed, in 5 h 06 min; peaks 3.6 to 6.2 GB
    (`cap_copy_batch`). About 100 more properties each than in part 3 (the
    free objects' check), and 30 to 50 % slower.

### Step 2, part 5: `map_query` (2026-10-27)

- **Settled** (the owner, 2026-10-27; 0020 amended): the caller names the
  frame. `map_query(task, va, n ≤ 64, frame or none, buffer)` reports per
  page: absent; or present, its permissions, its rights ceiling, and the
  page offset only for the frame named (with CONTROL). No other frame, no
  physical address; within one last-level table, as the other calls over
  pages. Rebuilding `mm` from it after a restart takes a pass per frame:
  open, for Phase 9.
- **Built:** `joist_map_query` (`map.c`), O(n), changing nothing;
  system call 46 (`ABI_MAP_QUERY`, the buffer in `R8`, written whole or
  not at all) and `j_map_query`; unit tests (the model's and the ABI's);
  the executor's operation, checked page by page against the hooks'
  record of what is mapped (present, permissions, a writable page's
  ceiling, the frame and offset exactly when named); CBMC operation 16
  (a frame is reported only through a capability with CONTROL);
  `growroot` queries a page it mapped, on the real kernel. Mutations:
  `query-shows-other-frames`, `-no-offset`, `-ceiling-lost`,
  `-frame-unchecked`, `-no-page-limit`.
- **Verified** (on the build VM, alone, the kernel built from nothing):
  - `make -C model check`: every test under plain, asan, ubsan, msan,
    tsan and small, no failure; the whole-model property test with a new
    floor (over 200 successful queries).
  - Mutations `query-`, `map-`, `lend-`, `hook-`: all 40 caught.
  - `make -C kernel check`: every host test (the ABI's query among them),
    freestanding build, clang-tidy and scan-build clean, BIOS and UEFI
    boots, all 74 self-tests (`growroot`'s query on 1 and 4 CPUs and on
    the checking kernel). The bench gate: 376, 1 568 and 1 112 cycles
    (baselines 496, 1 548, 1 092).
  - CBMC, operation 16: verified, 0 of 10 581 properties failed, 1 342 s.
    The other fourteen are unchanged by this part (part 4's run stands:
    the harness only gained the new operation).

### Step 2, part 6: TLA+ extended, and glibc's `fork` read (2026-10-27)

The order of work's item 3 asks TLA+'s dying-space rule extended to
mappings and to `cap_copy_batch`; the Testing table says the same.

- **`model/tla/IpcDelete.tla`:** the server's destruction gains its
  fourth step, its tables unmapped one at a time after its slots (as
  `drain_pending` does); clients may now map and unmap pages in its
  space and copy capabilities into it in a batch (several empty slots
  in one step). A new invariant, `TablesEmptied` (I12): when
  destruction ends, nothing is mapped. Mapping and the batch are refused
  into a dying space under `RefuseIntoDying`, as a single capability is
  (in the kernel, through the dead task capability).
- **TLA+ mutations** (`mutate_tla.py`), new: a batch copy into a dying
  space (`SpaceEmptied` breaks), a mapping into one (`TablesEmptied`
  breaks), a table sweep that passes a table without unmapping it, and
  one that may stall (`NoStrandedClient`).
- **glibc's `fork`** (the open item for step 3, 0022): it stops the
  parent's other threads for the whole capability copy (*Risks*), so the
  batch needs no stop of its own.
- **Verified** (on the build VM): `make -C model tla`: the safe
  configuration (5 876 distinct states) and the large one (1 144 512)
  hold every invariant and `NoStrandedClient`; the unsafe one
  (`RefuseIntoDying` false) leaks, as it must. `make -C model
  tla-mutate`: all 12 mutations caught, each by the property it breaks.
  The C model, the kernel and CBMC are unchanged by this part.

### Step 3, part 1: the fault message (2026-10-27)

The order of work's item 5, first half: the fault message gains what `mm`
needs, and a handler may do without the thread capability.

- **The message** (four registers, as every message: 0001's amendment of
  2026-10-03): `r[0]` the kind in its low byte and, for a page fault, the
  access in the next (read, write or execute, and whether the page was
  present), worked out by the kernel's trap code from the x86 error code
  so that `mm` never reads it; `r[1]` the address; `r[2]` the error code,
  as before; `r[3]` the faulting pc, new. `JOIST_FAULT_KIND`,
  `JOIST_FAULT_ACCESS` (`J_` in the user library).
- **No thread capability on request:** `thread_set_fault` and
  `task_set_fault` take flags; `JOIST_FAULT_NO_THREAD_CAP` makes page
  faults to that handler bring none (the badge names the task, the reply
  resumes the thread), sparing the handler a `cap_delete` per fault.
  Other faults still bring one. A fault call made so is marked
  (`faulting` 2), and delivery gives no capability; the checks bound both
  values and the flags.
- **Tests:** the model's unit tests (the pc and the access, the flag for
  the thread's handler and the task's, other kinds unaffected, unknown
  flags refused); the executor sets the flag at random; `faultroot` on
  the real kernel checks the pc and the access, and serves one faulter
  without a thread capability. Mutations: `fault-no-pc`, `-no-access`,
  `-nocap-ignored`, `-nocap-every-kind`, `-flags-unchecked`,
  `-task-flags-ignored`.
- **The fault round trip, measured** (`faultroot`'s `test_bench`: a
  `faulter` touches 256 fresh pages; the handler receives each fault, maps
  a page there and replies, deleting the thread capability if it got one;
  the faulter times each round trip with the TSC). On nested KVM (the
  build VM's QEMU inside the P53's), medians in cycles:

  | | with the capability | without |
  |---|---:|---:|
  | 1 CPU (`pcid-fault`) | 61 038 | 60 202 |
  | 2 CPUs, 6 runs (mean of medians) | 135 645 | 135 441 |
  | 4 CPUs | 136 052 | 135 874 |

  - Leaving out the thread capability saves nothing measurable here
    (within ±2 %, either sign): its cost, a capability insert and a
    delete, is lost in the round trip.
  - The round trip itself is large: 61 000 cycles on one CPU, 40 times an
    IPC round trip (1 572). The IPC benchmark's server-with-its-own-context
    row is of the same size, and the handler here has its own context, so
    the cost is probably the switch between scheduling contexts (a timer
    reprogrammed, which traps to the host under nested KVM). On more than
    one CPU the times fall in two groups: the fastest are the one-CPU
    figure, the median is about twice that, likely the wake-up of a thread
    on another CPU. Neither is checked yet, and no figure here is a
    baseline until it is measured on the P53 itself (not nested).
  - Data: `/srv/joist/handoff/p4-fault/findings.md` (the P53's Claude
    Code ran and recorded it).
- **Verified** (on the build VM, alone, the kernel built from nothing;
  CBMC, the last mutation and the second kernel check by the P53's Claude
  Code):
  - `make -C model check`: every test under plain, asan, ubsan, msan,
    tsan and small, no failure.
  - Mutations, the whole list: all 235 caught (`fault-flag-unset`, left
    stale by the change, retargeted and caught alone).
  - `make -C kernel check`: every host test, freestanding build,
    clang-tidy and scan-build clean, BIOS and UEFI boots, all 74
    self-tests (the 8 `fault` variants among them, with the benchmark).
    The bench gate: 372, 1 572 and 1 116 cycles (baselines 496, 1 548,
    1 092).
  - CBMC, all 15 operations: verified, 0 of about 10 560 (0-10) and
    10 655 (11-16) properties failed, 6.6 hours.

### Step 3, part 2: the fused reply decided, and the fault's cost (2026-10-28)

- **Measured on the P53 itself** (KVM, pinned cores; `docs/phase4-data/step3-fault/`):
  the fault round trip is about 11 000 cycles on one CPU (9 200 at best)
  and a median of about 25 500 on two (the fastest still near 11 000).
  Leaving out the thread capability saves nothing measurable.
- **No fused reply** (the owner, 2026-10-28): it would save one kernel
  entry, about 3 % of the round trip, so the plan's condition ("if the
  measurement shows the extra kernel entry matters") is not met. The cost
  is elsewhere: it is looked for first, before `mm` is built on the path.
- **Where the cost was** (`docs/phase4-data/step3-fault/`, *Where the time
  goes*): the handler's change of scheduling context, twice a fault (about
  4 200 cycles each, the timer set each time; a trap to the host under
  KVM). With a **passive** handler, running on the faulting thread's lent
  time and answering with ReplyRecv, a fault takes about 2 600 cycles on
  one CPU and 2 700 on two, against 10 500 and 25 300. `faultroot`
  measures all three now (own context with and without a thread
  capability, passive). For `mm` (item 6): run passive, on its clients'
  time; that is for the owner to settle with `mm`'s design.
- **Verified** (the build VM, alone, the kernel built from nothing):
  `make -C kernel check`: every host test, freestanding build,
  clang-tidy and scan-build clean, BIOS and UEFI boots, the 8 `fault` and
  7 `faultinj` variants with the new measurements; 73 of 74 self-tests.
  The one failure was `smp4-smp` (emulated, 4 CPUs: "spinners on separate
  CPUs didn't run at the same time"), which then failed once more in
  three reruns alone, with nothing else on the P53 or the VM (its cores
  as they should be). This is its fourth failure across runs, never under
  KVM: the emulated test's timing, not load. **Open, for the exit step:**
  a longer window, or a threshold that suits emulation. The bench gate:
  372, 1 572 and 1 116 cycles.
