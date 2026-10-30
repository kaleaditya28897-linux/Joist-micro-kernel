# Phase 4 exit

**Status: accepted by the project owner, 2026-10-30.** Phase 4 (§11,
*Memory management and `fork`*) is closed. The work, step by step, is in
`docs/phase4-plan.md`, *Progress*; data in `docs/phase4-data/`.

The final run of every suite is under *Verification*, at the end. It was
still running on the build VM when the owner accepted this record; its
rows are filled in as each suite finishes.

## Exit criterion

| Criterion (`phase4-plan.md`, *Testing*) | Met by | Evidence |
|---|---|---|
| `cowstress` passes N rounds on 1, 2 and 4 CPUs, under KVM on the P53 and on the checking kernel, every read verified and every counter level after every round (N = 1000, and 250 on the checking kernel: the owner, 2026-10-29) | `user/cowstress` and `user/cowworker`: fork trees 4-6 deep through `mm`, two writer threads per task, every page tagged and checked after every step; kills mid-fault and mid-fork; after every round no task but `mm` left, `mm`'s records sound, every page back, the kernel's counters as after the first round | **Passed** (2026-10-30, `tools/exit-run.sh` at f8d55b5, `docs/phase4-data/step7-exit-run/`). 1, 2 and 4 CPUs: 1000 rounds each, 14 865-15 043 forks, about a million writes and 62 003 verifications each, 4 264-4 331 kills mid-fault and 1 833-1 862 mid-fork. The checking kernel (every invariant on every exit), 2 CPUs: 250 rounds. `mm` stranded 0, slow 0, fork short of quota 0, in every run |
| The refinement property test passes with M seeds (M = 10 000; 1 000 under msan and tsan: the owner) | `model/test`: an abstract machine (each task an array of page values, `fork` a deep copy) against the concrete one (the kernel model, `mm`'s library and the fault calls), on random fork, read, write, unmap, protect, share, exit and kill | **Passed**, plain, asan and ubsan at 10 000 seeds, msan and tsan at 1 000: 453 690 reads checked, 168 495 COW copies, 137 301 clones at 10 000 |
| Every listed mutation is caught | `model/test/mutations.txt`: the kernel's core and `mm`'s library | **284 of 284 caught.** One (`mm-unmap-kernel-skipped`) had stopped building after part E8 and was rewritten to the same bug (`step7-exit-run/README.md`). The kernel's own list: 54 of 54 caught |
| Phase 3's suites, unchanged, in every full run | `make -C kernel check`, `make -C model check` | 87 of 87 self-tests (71 `run` lines; Phase 3's 71 runs among them) and every host test (2026-10-30, at 5ae1bd6) |

## What was built

| Scope (`phase4-plan.md`) | Status |
|---|---|
| Part of a frame mapped; page-sized mappings, the page tables the record (0020) | done (step 2): shadow pages, map counts, a dying frame's sweep on the destruction work list; I12 revised |
| `protect`, `unmap_range`, a conditional `map`, `map_query` | done (step 2, parts 1 and 5); `map_replace` for COW |
| Page tables paid for by memory `mm` donates (0021) | done: `space_donate`; `map` without enough lent pages is `E_NOMEM`, never a kernel allocation |
| Revoke by killing the object; the batched copy for `fork` (0022) | done (step 2, part 2); TLA+'s dying-space rule extended to mappings and the batched copy |
| The fault message completed; a fused reply, if measured worthwhile (0007) | message done (step 3: pc, access type, no thread capability on request). **No fused reply** (the owner, 2026-10-28 and 2026-10-29): a kernel entry is 3-9 % of a fault; `mm` maps runs instead (E6) |
| `servers/mm`: anonymous memory, demand-zero, COW clone, shared mappings (0023) | done (step 6 and parts E1-E9): passive, its logic in `mmlib` tested on the host against the model; fork confirmed (orphans), SHARE by both sides' consent, run-mapping |
| What a file-backed mapping does when its pager dies | decided (0024, accepted 2026-10-29); built in Phase 6 |
| The escape hatch of 0008 (the region capability) | designed (choice 7); **not built**: after the exit test, by the plan (*Carried forward*) |
| `fork` of a test program by `mm`; `posix_spawn`'s kernel side (an empty task) | done: `MM_REQ_CLONE` on an empty task made in O(1) |
| The `SELF` gap closed | done: mapping into one's own space needs a task capability to it with CONTROL (`docs/abi.md`) |
| Capacities raised for fork trees, compile-time constants | done (step 2, part 4): 64 spaces, 128 threads, 2 048 objects; objects found in O(1) |
| Guard pages under the double-fault, NMI and machine-check stacks | done (E10, 2026-10-30): one stack each per CPU, a guard page under each; found missing while drafting this record |
| `mm` restartable by design | designed (0023: its records rebuilt from its capabilities and `map_query`); restarting it is Phase 9's |
| A fallback scheduling context for passive servers (0025) | done (E4): `mm` stranded by a kill mid-request, found by `cowstress`, cannot happen; 0 stranded in the exit run |
| Toolchain trust (0026) | layer 1 (reproducible builds: `repro-check`, 32 of 32), layer 2 (a GCC from a full-source bootstrap, the seed read and accepted by the owner), layer 3 (DDC of Clang and lld: 270 of 270 files identical; DDC of nasm through Limine: 6 of 6) |

Decisions this phase: 0020, 0021, 0022, 0023 (amended twice, corrected
once), 0024, 0025 and 0026, all accepted.

## Measurements

All under KVM; bare metal is still not available.

**The fault path, in the kernel** (step 3, the P53, pinned cores,
`docs/phase4-data/step3-fault/`): a round trip through a handler with its
own scheduling context, about 11 000 cycles (1 CPU) and 25 500 (2 CPUs),
nearly all of it the two context changes (each writes the timer, a trap to
the host under KVM). Through a **passive** handler, as `mm` is: about
2 600 cycles, 2 700 on 2 CPUs.

**`mm`, on the P53** (2026-10-30, `docs/phase4-data/step7-p53/`, its
`bench.sh`: 3 boots each, pinned cores; medians in cycles, 1 CPU):
`nosenders_next` with nothing queued 274-288; a request 1 326-1 360; a
write fault that upgrades 3 290-3 390; a demand-zero write fault
10 070-10 300; a copy 13 040-13 390; a demand-zero read fault (16 pages)
53 000-54 300; `protect` of 512 pages 22 200-24 600; SHARE of 64 new pages
about 221 000-225 000; CLONE of 1 MiB 65 300-67 000. On 2 CPUs within a
few per cent of these. The large build (1 CPU): CLONE of 16 MiB about
1.38 M, of 256 MiB about 10.0 M (its 1 MiB clone 785 000: the large
build's task records are cleared whole, E2). E6's figures on the nested
build VM are close to these.

## Found by the review of `mm`

The plan (*Risks*) makes a review of `mm`'s authority part of the exit;
`mm` is in the trusted base.

- **What it holds:** its endpoint (RECV, SEND, GRANT), a task capability
  to itself (to map its pool), its pool frame, a frame of pages to lend
  for page tables, a task capability (CONTROL) to each task it serves,
  and the root's grant of those. Nothing ambient.
- **What a client holds:** one capability to `mm`, SEND only, badged with
  its index. It can't pass it on (no GRANT), so a fork child gets its own
  from `mm`. A client's capabilities sent to `mm` are dropped: `mm` offers
  no receive slots, so a client can't fill `mm`'s space.
- **What a client may ask:** every request is checked against the badge;
  the root's three requests only from the root's badge; a fault from a
  badge `mm` doesn't serve is not resumed. Ids carry no authority (SHARE
  needs the target's acceptance; CLONE_DONE only the parent's own pending
  child). A client's own CONTROL over a child it forked lets it break that
  child's mappings, but only that child's.
- **Found: `mm` has no per-client limit.** A task can map up to
  `MM_TASK_PAGES` (512) pages, as many as the whole pool (512), and fork
  until `mm`'s records (`MM_MAX_TASKS`, 16) are full. One client, or one
  fork tree, can take every page and every record; after that every other
  client's demand-zero or copy fault is refused and its thread ends. This
  is denial of service by exhaustion, not a breach of isolation (no
  client can read or write another's pages). Not covered by 0023 or the
  plan. *For the owner*, below.

## Found along the way

- **A passive `mm` stranded by a kill** (E1): killing a client while `mm`
  ran on its time took that time back, and `mm` stopped mid-request for
  ever. Decision 0025: a fallback scheduling context.
- **Orphaned children** (E1, E5): a child whose parent died between CLONE
  and its start was named only by `mm`. Now a fork is confirmed
  (`MM_REQ_CLONE_DONE`), and a parent gone takes its pending children.
- **A fork child's quota** (E9, found by the exit run's first minute):
  each live child's thread is charged to its parent; 8 was too few for
  the trees `cowstress` grows. Now 32, and a fork the quota can't pay for
  is cancelled.
- **A slow `mm` is not a stranded one** (E7): on squeezed CPUs `mm` missed
  `cowstress`'s 300 ms; it now waits up to 10 s and counts the slow ones.
- **Builds broke in a path with spaces** (5b47b0d): the unquoted
  `-ffile-prefix-map`.
- **The guard pages under the IST stacks** were in the scope but not
  built (E10): found while drafting this record.
- **The fuzzer's self-check had checked nothing since step 6**
  (`fuzz-sanity`, found by the final run): it copied the model without
  `servers/mm`, so none of its planted bugs built. It now copies it, as
  `mutate.sh` does, and catches all 7.
- **The x87/SSE2 mismatch** in the first DDC comparison (0026): the
  bootstrapped GCC's i686 default is x87; every stage now uses SSE2.

## Decided at the exit (the owner, 2026-10-30)

- **This record accepted,** and Phase 4 closed.
- **`mm`'s per-client limit** (the review, above): left to Phase 7's
  process server, which knows users and sessions and sets `RLIMIT_AS` and
  `RLIMIT_NPROC`. `mm` has no notion of whom a task belongs to, so a limit
  it set now would be replaced. The gap is written into 0023 (amendment of
  2026-10-30). The other option was a per-task page limit in `mm`,
  inherited by a fork child and charged to the tree's root.

**Not decided at the exit** (carried forward, below):

- **The region capability** (choice 7, the plan's item 8): built before
  Phase 5 (it is also Phase 5's receive window), or first in Phase 5.
- **The `smp4-smp` emulated flake** (step 3): four failures across runs,
  never under KVM, the emulated test's timing. Widen its window under
  emulation, or leave it.

## Carried forward / open

- **Bare metal** is still not available.
- **CBMC for `obj_kill` and `cap_delete` from states with mapped frames**
  (the dying frame's sweep): not done; covered by property tests and
  mutations.
- **`map-sweep-skips-top`** is caught by the general I12 count, not by a
  test aimed at the top-level stride.
- **`mm` restarted** (class C): Phase 9.
- **A fork child's quota** (32) and `mm`'s per-client limit: policy for
  the process server (Phase 7; 0023's amendment of 2026-10-30). Until then
  one client can exhaust `mm`'s pages and records.
- **The region capability** (the plan's item 8): not built; when (before
  Phase 5, or first in it) is still the owner's to decide.
- **The `smp4-smp` emulated flake:** open; widen its window under
  emulation, or leave it, is still the owner's to decide.
- **File-backed memory** (0024's rule) and the paging protocol: Phase 6.
- **0026, still proposed:** Limine's host programs and fixed build
  directory; the verified toolchain and Limine from source as Joist's
  defaults.
- **To Phase 5:** moves and grants (0006), with choice 9's rule (the kernel
  moves only pages in region ranges, which `mm` never COW-shares).

## The kernel at exit

Counted as Phase 3's record counts: "Code" is non-blank, non-comment lines;
"All" is `wc -l`. `.c` and `.h` files; Limine's header, the linker script,
tests and fuzz harnesses left out.

| | Code | All | Phase 3's exit (code) |
|---|---:|---:|---:|
| `kernel/core` (the object layer) | 4 589 | 5 387 | 3 533 |
| `kernel/include/joist` | 476 | 1 214 | 399 |
| `kernel/mem` | 589 | 780 | 492 |
| `kernel/sys` | 582 | 758 | 536 |
| `kernel/elf` | 128 | 170 | 128 |
| `kernel/lib` | 79 | 92 | 79 |
| **Portable C** | **6 443** | **8 401** | **5 167** |
| `kernel/arch/x86_64` (C) | 2 309 | 3 016 | 2 176 |
| Assembly (`isr.S`, `syscall.S`) | 130 | 184 | 130 |
| **Kernel total** | **8 882** | **11 601** | **7 473** |
| `servers/mm` (not the kernel) | 1 189 | 1 537 | — |

Against the tripwire (§5.1: ~15 000 lines of C in the core kernel,
excluding arch-specific code): portable C is 6 443. The plan's estimate
(*Risks*): "1 500-2 500 lines: about 10 000 of the 15 000"; Phase 4 added
1 409 lines of code to the whole kernel.

| | |
|---|---|
| Model tests | 247 (`JT_TEST` in `model/test/*.c`, 27 files), under plain, ASan, UBSan, MSan, TSan and the small build; CBMC on 19 operations; TLA+ |
| Kernel host tests | 54 (`JT_TEST` in `kernel/test/*.c`), in five builds; libFuzzer on the ABI dispatcher, the ELF validator and the system-call layer |
| Boot self-tests | 87 runs in `kernel/test/selftest.sh` (71 `run` lines, some in loops) |
| Mutations | 284 in `model/test/mutations.txt`, 54 in `kernel/test/mutations.txt` |
| User programs | 29 in `user/`: Phase 3's 23, and `revokeroot`, `mmroot`, `mmclient`, `cowstress`, `cowworker`, `mmbench`; `servers/mm` |
| Toolchain | Clang/LLD 21.1.8 (verified by DDC, 0026), Limine 12.9.2 (built from source with a nasm checked by DDC), CBMC 6.11, TLA+ tools v1.7.4; QEMU 11.1.2 |
| Machines | the build VM `joist-dev2` on the P53; real-CPU boots on the P53 (i7-9850H), pinned; the workstation (i7-8665U) since 2026-10-30 |

## Verification

The final run of every suite on the tree of this record, on the build VM
(2026-10-30), except where said:

| Suite | Result |
|---|---|
| model check (plain, ASan, UBSan, MSan, TSan, small) | 6 × 247 tests, 0 failed |
| model mutate | 284 of 284 caught (the workstation, *Exit criterion*) |
| model cbmc | running on the build VM |
| model tla, tla-mutate | safe and large configurations hold (large: 1 144 512 distinct states); the unsafe one leaks, as expected; every TLA+ mutation caught |
| model fuzz, fuzz-sanity | passed: each of the 7 planted bugs caught; the fuzzer 60 s clean. The first run failed: `fuzz-sanity.sh` didn't copy `servers/mm`, which the model has needed since step 6, so no mutant built (fixed; *Found along the way*) |
| kernel check | passed at 5ae1bd6: 87 of 87 self-tests, every host test in five builds, `tidy` and `scan` clean, BIOS and UEFI boots, `repro-check` 32 of 32 |
| kernel mutate | 54 of 54 caught |
| kernel fuzz-abi, fuzz-elf, fuzz-syscall | passed, 60 s each |
| The exit run | passed (*Exit criterion*) |
