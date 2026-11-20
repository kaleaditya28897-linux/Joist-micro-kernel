# Phase 5 exit

**Status: complete, for the owner's acceptance (2026-11-20).** Phase 5
(§11, *MIG retarget and libports*): its three exit criteria met, its scope
built (move-grant replaced by 0032, the owner's decision), every suite
passing (*Verification*). The work, step by step, is in
`docs/phase5-plan.md`, *Progress*; data in `docs/phase5-data/`.

## Exit criterion

| Criterion (`joist-microkernel-plan.md` §11, Phase 5) | Met by | Evidence |
|---|---|---|
| Unmodified-logic Hurd server code runs on the new kernel via retargeted stubs | The Hurd's `trans/hello`, its source unchanged, on libtrivfs, libports, libiohelp, libfshelp and glibc's `sysdeps/joist`, its stubs joistmig's; libtrivfs and libports with a few Joist files laid over them (`hurd/joist/`), their logic otherwise the Hurd's | **Met** (step 10). The root attaches it (`fsys_startup`), takes its root node (`fsys_getroot`), reads, seeks, stats it, has a write refused, and kills it while a call waits in it, the root left clean: selftests `trivfs`, `smp2-trivfs`, `smp4-trivfs`, `kvm-smp1/2/4-trivfs`. libports' test server under fault injection: 21 of 21 (seeds 1-7 on 1, 2, 4 CPUs, step 8) |
| The region capability built, its calls in the model with CBMC and mutations, and measured against plain RPCs to `mm` | 0030: the kernel object and its five calls, `mm` making, growing and killing regions | **Met** (steps 2-3). Property tests under every sanitizer; CBMC on operations 22-26 (`region_map` 6 018 s); region mutations caught. Measured on the P53: a `malloc`-like churn 2 to 8.7 times cheaper than requests to `mm` (`docs/phase5-data/step3-region-churn/`) |
| A fork bomb and a memory hog, each in its own tree, leave a third task able to fault new pages and fork | 0028: `mm`'s limits per task tree | **Met** (step 4). `mmroot`'s trees test: a fork bomb stopped at its tree's task ceiling, two hogs at their page ceilings, a fifth tree refused at admission, while a fourth tree faults and forks, never refused ("mm: trees ok", in every kernel check) |

## What was built

| Scope (`phase5-plan.md`) | Status |
|---|---|
| The region capability (0030): kernel object and calls, `mm` making and killing regions, measured | done (steps 2-3) |
| `mm`'s limits per task tree (0028) | done (step 4); `mm` refuses `MM_REQ_INIT` without a table run for every task record (the owner, 2026-11-04) |
| A MIG backend for Joist's IPC, `.defs` unchanged | done (step 6): joistmig, MIG 1.8 with a Joist output mode (`third_party/mig/`); every Hurd RPC stub in glibc is joistmig's; MIG's destructors, `<demuxer>_routine`, `dealloc[]` results, null ports (steps 8-10) |
| OOL: in line, through the IPC buffer, and move-grant (0006) | move-grant **replaced** by 0032 (the owner, 2026-11-14): a kernel copy through windows the caller gives (`thread_window`, `reply_read`, `reply_write`), in the model with CBMC and mutations, in the kernel's calls, in joistmig's stubs, and glibc's window memory (one region a task, slices a call) |
| libports on endpoints, badges and lifetime notifications | done (step 8): badges as serials in a table of live ports, `ReplyRecv` loops, a bounded pool, a no-senders thread a bucket judged by glibc's make-send count, cancel as a flag |
| glibc's `sysdeps/joist`, static: the Mach layer, `htl` threads, `malloc` over a region (0031) | the Mach layer and `htl`: done (step 7; glibc's own tests below). `malloc` over a region: **found missing at the exit and built then** (the owner, 2026-11-18): the main arena's break (`brk`, `sbrk`: `MORECORE`) in one region from `mm`, 8 MiB held from start-up, grown by doubling extents from 64 KiB; past it, and for blocks above 128 KiB (no dynamic `mmap` threshold), `mmap`. Thread arenas stay on `mmap`, since each is a 64 MiB heap aligned to 64 MiB, a region's most. glibctest's stress: 200 000 mallocs and frees of 1 B to 4 KiB, every block in the region |
| libtrivfs and `hello`, started by the root and called by a test client | done (step 10) |
| The review of the `dealloc` OOL sites (0006) | done (step 9): 56, not 54; no sender uses its memory after sending (`docs/phase5-data/step9-dealloc/`) |
| CBMC for `obj_kill`/`cap_delete` with mapped frames; the sweep's stride test | the stride test done; CBMC with page tables too large (11.8 GB on the build VM, 18 GB on the workstation): **closed as a known limit** (the owner, 2026-11-03) |

Decisions this phase: 0030, 0031, 0032 (amending 0006), all accepted; 0028
built.

## Measurements

All on the P53's own CPU (KVM, the virtual CPUs pinned):

| What | Result | Data |
|---|---|---|
| Region churn against requests to `mm` (map, write, unmap of 1-64 pages) | region calls 2 to 8.7 times cheaper | `step3-region-churn/` |
| The FPU's share of a round trip | about 290 cycles a round trip; the bench gate's baselines | `step7-fpu-ipc/` |
| `reply_write` of 4, 16, 64 KiB (the copy's cost; the kernel lock's hold at `JOIST_COPY_MAX`) | about 0.1 cycle a byte from cache; 64 KiB holds the lock about 6 800 cycles (2.6 µs); 64 KiB kept (the owner) | `step9-copy/` |
| `io_read` from glibc, 2 KiB to 1 MiB | about 22 000 TSC counts plus 0.16 a byte (1 CPU): 24 832 at 16 KiB through the window, 188 506 at 1 MiB. Warm copies; cold ones left to the first file server | `step10-ioread/` |
| A MIG round trip against the raw IPC one | `io_seek` to `hello`: 3 198 TSC counts (1 CPU), about 2.9 times the raw round trip (about 1 110) | `step11-mig-roundtrip/` |

## Found along the way

- **Kernel:**
  - A thread killed while blocked in a call left `abi.c`'s `pending[]` set,
    and a thread spawned into its slot got the dead call's outputs in its
    start registers. Found by libports' test; fixed in f4f509b.
  - Under KVM in KVM the TSC was seen to stand still while the timer's
    interrupts went on. Fixed in 951bedb (the clock moves to each deadline
    the timer was set for).
  - 951bedb's fix had its own bug: a stale timer interrupt moved the clock
    to a deadline not yet reached. The P53 hand-off bisected it; fixed in
    a82af70.
  - The kernel's `memcpy` was a byte loop (1.2 cycles a byte); it is now
    `rep movsb` (65bdae8).
- **glibc on Joist:**
  - A server thread's waits returned at once while it served a call (the
    kernel's I10); its reply is now saved first.
  - Answers sometimes went to the wrong caller, and one wake-up was lost
    (step 8).
  - A no-senders notice taken while a send right was being handed out was
    acted on; it is now judged by Mach's make-send count, kept in the
    names table.
- **The Hurd's code:** a race in `hurd/hurdlock.c` (robust mutexes). Fixed
  in Joist's copy; a bug report was written for the Hurd.
- **joistmig:**
  - an argument named like the stub's own variables broke the build;
  - `dealloc[]` results were freed through a NULL flag;
  - null ports couldn't cross: the kernel has no empty capability
    position. They now cross as marks in the message's first register.
- **Phase 0's list of OOL parameters** was 2 short (a type defined inside a
  parameter).
- **CBMC's harness** compared too large a state to finish (46 minutes);
  its "changes nothing" checks are the unit and property tests'.
- **At the exit:**
  - `malloc` over a region had never been built (above).
  - Its first form, a 1 MiB first extent, took all of `hello`'s pages in
    the selftests, and `hello` died before `fsys_startup`: an extent's
    frames are taken whole, where Mach's `brk` takes only pages touched.
  - With the break capped, glibc's dynamic `mmap` threshold sent large
    blocks into the main arena past it, which `malloc` never gives back
    (`stdlib/tst-qsort2` reached its 32 MiB).
  - Step 10's selftests had taken the names of Phase 1's `hello`
    selftests; renamed `trivfs`.
  - The mig-test's Debian package `gnumach-dev_1.8+git20261028-1` is gone
    from Debian's ports archive: the run uses the copy already checked
    (its SHA-256 in `tools/mig-test/run.sh`).

## Decided at the exit

- **`malloc` over a region** (0031, step 7's design item 6, the
  *Testing* row's "`malloc` stress in a region"), found not built: built
  before the exit (the owner, 2026-11-18), the main arena only (the owner,
  the same day). Thread arenas' heaps in regions would need a smaller
  `HEAP_MAX_SIZE` for Joist and more of a task's 8 regions: Phase 7's, if
  measured worth it.
- **The break's extents:** 64 KiB doubling to 8 MiB, at most twice what
  the break reached taken from `mm` (the owner, 2026-11-18; a 1 MiB first
  extent and 64 MiB reach set aside).
- **No dynamic `mmap` threshold** on Joist (the owner, 2026-11-20): glibc
  patch 0013 and `sysdeps/joist/Makefile`.
- **glibc's test-suite figures** (below) accepted as the phase's record
  (the owner, 2026-11-20).

## Carried forward / open

- **Messages:**
  - Deferred replies (`MIG_NO_REPLY` via `reply_save`) in joistmig's
    server loops.
  - No cancel threshold, since Joist messages have no sequence numbers.
- **glibc:**
  - `read` past 8 MiB (`JOIST_MIG_WINDOW_MAX`) on a regular file comes back
    short. `fd-read.c` must loop for regular files once a file server
    exists.
  - `fd-write.c` could raise its 4 KiB cap.
  - Phase 7's `fork` must take the window lock.
  - Joist's own locks in glibc must become critical sections before a
    thread can be aborted mid-call (Phase 7).
  - Robust mutexes can't notice a thread that died holding one
    (`htl/tst-robust1`, `4`, `7`, `9` wait for `EOWNERDEAD` and time out).
  - The break's pages stay taken when it comes down (a region can't give
    extents back), and a program's heap past 8 MiB is `mmap`'d pieces
    `malloc` never returns.
  - htl leaks a stack per thread (about 120 threads a lifetime).
- **Hurd libraries:** files using GCC's nested functions don't compile
  (libshouldbeinlibc 5, libfshelp 3; `portinfo.c` lacks a stub header).
  `hello` reaches none of them, but Phase 8's servers will.
- **Measurements:**
  - Cold copies through the windows, and `JOIST_COPY_MAX` checked again,
    with the first file server (Phase 6).
  - Where a MIG round trip's 3 600 cycles go (Phase 11).
- **Tests:**
  - libports' loops and notifier against the kernel model on the host
    (design item 8: only the names table's count is host-tested).
  - CBMC with page tables on kill and delete stays a known limit.
- **The kernel's clock:** the TSC isn't trusted where CPUID doesn't call
  it invariant (as on joist-dev2's guests). The timer's interrupts keep
  deadlines met there.

## The kernel at exit

Counted as Phase 4's record counts (non-blank, non-comment lines; `.c`,
`.h`; the counter reproduces Phase 4's figures at its record's commit).
The exit's own work (the break region, the selftests) changed no kernel
code.

| | Code | All | Phase 4's exit (code) |
|---|---:|---:|---:|
| `kernel/core` (the object layer) | 5 340 | 6 269 | 4 589 |
| `kernel/include/joist` | 537 | 1 371 | 476 |
| `kernel/mem` | 589 | 784 | 589 |
| `kernel/sys` | 635 | 832 | 582 |
| `kernel/elf` | 151 | 201 | 128 |
| `kernel/lib` | 83 | 102 | 79 |
| **Portable C** | **7 335** | **9 559** | **6 443** |
| `kernel/arch/x86_64` (C) | 2 423 | 3 207 | 2 309 |
| Assembly (`isr.S`, `syscall.S`) | 130 | 186 | 130 |
| **Kernel total** | **9 888** | **12 952** | **8 882** |
| `servers/mm` (not the kernel) | 2 037 | 2 561 | 1 189 |
| `glibc/sysdeps/joist` (not the kernel) | 5 338 | 8 502 | — |
| `hurd/joist` (Joist's files over the Hurd's) | 1 092 | 2 010 | — |
| `third_party/mig/joistmig.c` | 1 291 | 1 505 | — |

Against the tripwire (§5.1: about 15 000 lines of C in the core kernel,
excluding arch-specific code), portable C is 7 335, 892 more than at Phase
4's exit.

| | |
|---|---|
| Model tests | 306 (`JT_TEST` in 29 files), under plain, ASan, UBSan, MSan, TSan and the small build; CBMC on 27 operations |
| Kernel host tests | 59, in five builds |
| Boot self-tests | 78 `run` lines in `kernel/test/selftest.sh` |
| Mutations | 372 in `model/test`, 61 in `kernel/test`, 34 in `tools/mig-test`, 41 in `glibc/test` |
| User programs | 31 in `user/` (Phase 4's 29 and `glibcroot`, `glibctest`); `servers/mm`; the Hurd's `trans/hello`, built by `tools/phase5/hello.sh` |

## Verification

On 6816e03 and the exit's last change (glibc patch 0013), joist-dev2
(KVM) unless said:

| Suite | Result |
|---|---|
| `make -C kernel check` (the laptop; `GLIBCTEST`, `HELLO` set) | host tests 59 of 59 in each of five builds; every boot selftest ok, `glibc`, `trivfs` and `hello` among them (on 0013 again: the 15 glibc, `trivfs` and `hello` runs ok) |
| `make -C kernel mutate` | 61 of 61 caught |
| `make -C model check` | 306 tests, 0 failed, under plain, ASan, UBSan, MSan, TSan and the small build |
| `make -C model mutate` | 372 of 372 caught |
| `make -C model cbmc` | all 27 operations verified (14.7 hours) |
| `glibc/test` check and mutate | `test-vm` 2 000 seeds ok, `test-gsync` ok; 41 of 41 caught |
| mig-test: round trips, fuzz, mutate | 74 000 calls over 1 000 seeds, 0 failed; 5.9 million fuzz runs in 600 s, no crash; 34 of 34 caught |
| libports' test server, fault injection (built against the exit's glibc) | 21 of 21: seeds 1-7 on 1, 2 and 4 CPUs |
| glibc's own tests (`phase5-data/step11-glibc-tests/`) | 1 938 PASS, 41 UNSUPPORTED, 1 003 FAIL, 21 TIMEOUT, 57 NO-STATUS of 3 060 (step 7: 1 937, 41, 1 010, 16, 56); every change explained |

The exit run's first kernel check failed on the selftest names step 10
took twice; renamed, it passed. Its mig-test steps failed on the missing
Debian package; with the checked copy they passed.
