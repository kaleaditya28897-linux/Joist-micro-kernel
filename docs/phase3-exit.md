# Phase 3 exit

**Status: for the project owner's review** (2026-10-15). Drafted by the P53's Claude Code, completed by this session. Phase 3
(§11, *IPC and capabilities*). The work, step by step, is in
`docs/phase3-plan.md`, *Progress*; data in `docs/phase3-data/`.

The final verification run of every suite is under *Verification*, at the
end.

## Exit criterion

| Criterion (§11) | Met by | Evidence |
|---|---|---|
| IPC benchmarks stable | `user/ipcbench`, run by `user/benchroot`: round trips in CPU cycles (by the benchmark's own ruler), one and two address spaces, passive server and server with its own context, and a do-nothing system call. *Stable* (plan, *Testing*): across 10 runs the median moves under 5 % and the 99th percentile under 20 %. A gate in `make check` (`bench-gate` in `kernel/test/selftest.sh`) fails a median more than 25 % above `kernel/test/ipcbench.baseline` 10 runs each of the fast-path and slow-path kernels on the P53 (i7-9850H, KVM, 4 vCPUs pinned, QEMU 11.1.2; `docs/phase3-data/README.md`, *Step 10: the exit benchmark*). **Every uncontended round trip passes**: medians move 0-2.3 %, 99th percentiles 2.6-16 %. **Two figures do not**: the do-nothing call's 99th percentile on the fast path (29.4 %: two runs of ten read 736-740 against 572), and the contended line's median (8.5 % fast, 7.9 % slow), which is the one kernel lock's (0017). The gate's baseline was measured again on the same tree: 496, 1 548, 1 092 |
| Fault-injected client/server tests pass | `user/faultinj` and `user/fiworker`: five scenarios (a client in a call, a server holding a reply, a saved reply, capabilities in transit, a thread signalling or waiting), 13 kill points, five ways to kill (thread kill, last thread capability, task destroy, last task capability, reply object or capability deleted). Survivors checked against `docs/abi.md` (`E_PEER`, `E_GONE`, withdrawn messages, no-senders and dead names exactly once, skipped capabilities, signals as sent); every wait bounded; kernel counters level after every round. Seeded, replayable | Seven self-tests, seeds 1-7, 100 rounds each (1 CPU twice; 2 and 4 emulated; 2 and 4 under KVM; the checking kernel on 4 under KVM), 13 of 13 kill points in each. 9 000 more rounds (seeds 11-16, 300 rounds, five machine shapes). No kernel or core bug found. Six bugs planted in the core by hand, one at a time, each caught at the first round to reach them. Recorded in `docs/phase3-plan.md`, *Step 10, part 3*, and in the hand-off report `/srv/joist/handoff/faultinj/findings.md` on the P53; not in `docs/phase3-data/` |
| No kernel memory growth under adversarial tests | `debug_counters` (call 3, root task only) reports live objects, thread records, spaces, table leaves, mappings, destructions pending, free page-table frames and free boot frames. `user/growroot` makes and destroys tasks, threads, endpoints, notifications, contexts, capabilities and cycles of tasks (reaped), fills the object, space and mapping tables to `E_NOMEM`, and runs `user/fuzzchild` (random system calls) in its tasks. After the first round every counter must read the same after every round | 41 rounds on 4 CPUs under KVM, 60 cycles reaped, every counter unchanged; 9 rounds emulated and on the checking kernel (self-tests `grow`, `kvm-smp4-grow40`, `chk-kvm-smp4-grow`). A kernel made to leak one page table in fifty, tried once by hand, fails at the second round. `faultinj` checks the same counters after every round. Recorded in `docs/phase3-plan.md`, *Step 10, part 1*; not in `docs/phase3-data/` |

Step 8's record (`docs/phase3-data/README.md`, *Step 8*; files
`step8/vm112-ipcbench-fast-10runs.txt`, `step8/vm112-ipcbench-slow-10runs.txt`).
VM 112, one vCPU on a P-core of its own, 20 segments of 100 000 calls, ten
runs. CPU cycles per round trip:

| | Fast path: median | 99th percentile | Slow path only: median |
|---|---|---|---|
| A system call that does nothing | 372-376 | 416-428 | 580-584 |
| Two address spaces, passive server | 1 356 | 1 480-1 508 | 1 896 |
| One address space, passive server | 764 | 852-860 | 1 292-1 296 |
| Two address spaces, server with its own context | 9 732-9 764 | 9 932-10 908 | 9 992-10 020 |

Medians move 1.1 % at most; 99th percentiles 9.8 % (fast) and 11.7 %
(slow), both on the last row, under 2 % on the three passive rows. Inside
the plan's measure. The exit benchmark on the P53 (step 10), fast path,
medians of 10 runs: 528, 1 624, 1 164 and 13 808 cycles for these four
rows, 2 976 with 4 KiB of IPC buffer each way, 9 072 with every other CPU
making system calls; slow path 836, 3 168, 2 712, 14 640, 4 532, 10 768.
They are higher than step 8's for three reasons, each measured: a
different CPU, `ipcbench` now taking off the clock's real cost rather
than an underestimate of it, and the run queues (172 cycles a round trip).

## Activities

| Activity (§11, `phase3-plan.md` *Scope*) | Status |
|---|---|
| Capabilities | done; the two-level table (0001 amendment, item 4): 64 × 128 = 8 192 slots, O(1) lookup, leaves charged to quota |
| Endpoints, synchronous IPC, reply capabilities | done, with reply objects for deferred replies |
| Fast path | done: a direct switch after the core's own operation, for `Call` and `ReplyRecv`; the core stays the only implementation. Checked by a differential test against the scheduler (`model/test/test_handover.c`) |
| Notifications, lifetime notifications | done (no-senders, dead names); exercised under fault injection |
| Priority / scheduling-context donation | done: 256 priorities, contexts with budget and period, donation on `Call`, chains, a one-shot timer |
| Benchmark round-trip latency in QEMU and on real hardware | done with a departure: the real CPU is under KVM (VM 112, then the P53), never bare metal; under TCG the benchmark runs in `make check` (`bench`) but its figures are not recorded |
| Record in CI | done with a departure: there is no CI service; the gate is part of `make check` in `kernel/`, on the real CPU under KVM |
| One object layer, two builds | done (step 2): `model/src` is `kernel/core/` |
| The binary system-call ABI | done (step 4; `docs/abi.md`, *Encoding*): 46 call numbers in `kernel/sys/abi.h` today, message registers in R12-R15, capabilities and word count in R9 |
| Many tasks and threads, address spaces, context switch | done (step 4); the kernel half shared by construction; PCID (step 8) |
| Frame capabilities, minimal `map`/`unmap` | done (steps 3 and 4); the root task gets an 8 MiB pool and a frame per module |
| Capability transfer in messages | done (step 3): up to 2 per message, never blocks or allocates |
| The 4 KiB IPC buffer | done late (step 10, part 2): up to 512 words each way, call 72 |
| Fixed-priority scheduler, O(1) | done (step 10, after the owner withdrew acceptance of the scans): a run queue per CPU, a list per priority and a bitmap; choosing costs O(CPUs), not O(threads). The scan stays as the specification the invariant checker holds the queues to. Cost: 172 cycles a round trip (+12.5 %), 24 a do-nothing call |
| `Yield` (0014) | done (step 10): call 54, behind equals, the budget untouched |
| Fault IPC (0009), thread control (0010) | done (steps 3 and 6), with registers through the caller's memory (calls 70, 71) |
| Preemptible cascades | done (step 7): at most 32 slots per operation (`RUN_PREEMPT_SLOTS`), a piece per kernel entry, the rest in idle |
| SMP under 0016 | done (step 9): up to 4 CPUs, one ticket lock, two kinds of IPI, global scheduling |
| `-fstack-protector-strong` (step 10) | done: canary from RDRAND where present; self-test 9 |
| Guard pages under kernel stacks (step 10) | done, under each CPU's kernel stack only; self-test 8 |
| Fault-injected tests; the memory-growth test | done (step 10) |
| Revoke, 0011's batched copy; grants; IRQ/I/O/MMIO capabilities | not in Phase 3, by the plan (Phases 4, 5, 10) |

## Found along the way

- **CBMC found a quota overflow** (step 3, part 1). A fixture with a quota
  of `UINT32_MAX` wrapped on refund. Now a budget must fit 32 bits, checked
  by the invariant checker.
- **Kernel time was charged to no one** (step 5, part 4). Seen only on the
  real CPU, where entry and exit cost as much as a short request. A thread
  is now charged from the kernel entry that led to its resumption. The
  scheduler test gained *kernel time*, which the old kernel fails on VM 112.
- **A timer write on every exit** (step 5, part 5). Under KVM each write
  leaves the guest: 2.5 µs a round trip against 0.7. The timer is now
  written only when its moment changes.
- **The place-in-line rule** (step 8). A call to a passive server gave up
  the caller's turn, which nobody intended. A context's place among its
  equals now goes with it. The differential test caught the first version,
  which copied the place instead of moving it.
- **Cascade sweeping in the wrong name** (step 7). The first rule swept on
  while the entering thread still ran; a timer interrupt during another
  thread made that thread pay. Replaced by one piece per entry.
- **The evict/restart rule** (step 9). A thread on another CPU that is
  stopped, read, written or destroyed is brought into the kernel first; a
  system call it was entering is backed up to its `syscall` instruction and
  made again later.
- **No guard page under kernel stacks** (step 9). The checking build ran
  through a 16 KiB stack into the per-CPU records. Guard pages came in
  step 10.
- **Benchmark noise on a shared host** (step 8). Turbo steps against a
  fixed-rate TSC moved medians 30 %. The benchmark now measures in CPU
  cycles against a ruler, waits a second, and takes the median of 20
  segments. The fourth measurement had printed the histogram's top edge
  (16 388) once it passed 16 384 cycles; its buckets now come from the
  warm-up.
- **The null system call's two clusters** on the P53 (336-352 cycles in 16
  of 20 runs, 444-476 in 4) were `ipcbench`'s own: it measured the clock's
  cost once, while the CPU was still slow from idle, and took that off every
  sample. The high group was the true one. It now measures the clock's cost
  with every batch; 10 runs of the gate read 492-496.
- **The run queues' first version cost 352 cycles a round trip** (+26 %):
  a Call put the server on a queue and took it off twice. Holding the queues
  for a whole system call and filing each changed thread once, after the
  direct switch, brought it to 172. Record layout, jump alignment, the
  system-call layer and a lost fast path were each measured and ruled out
  on the way.
- **`benchroot` could hang on 4 CPUs** (the lock study): it started passive
  servers without the start-up handshake `docs/abi.md` requires on SMP.
- **Step 10's first full run failed** on a mutation made stale by part 2
  (the per-part cadence runs only a part's new mutations) and on the
  checking kernel's IPC-buffer test outliving its time limit. Both fixed;
  `mutate.sh` now checks every mutation's text even in a filtered run.
- **CBMC did not run** in the full runs of steps 7 and 8, though reported
  as verified (*Correction*). `gcc` was missing on the new build VM, and a
  pipe through `tail` hid the status. CBMC was then run on step 8's tree
  (7 of 7). `tools/dev/fullrun.sh` now records each suite's own status.
- **SeaBIOS with SMM resets guests on the P53** under KVM, at GRUB's
  hand-over (`docs/testbed.md`). Linux and Hurd boot with `smm=off`.
  Joist's own BIOS boot (Limine) is not affected. Cause not pursued.
- **A CPU's first exit overwrote its own stack frame** (found by 0017's
  timer change): `on_own_stack()` started a CPU's code at the very top of
  its stack, where `enter()` builds the frame it returns through. It worked
  only while the compiler kept `enter()`'s thread number in a register.
  Fixed: a fresh stack starts one trap frame below the top.
- **The IPC buffer was missing until step 10.** It was in the scope table
  and choice 8 but not built; writing this record found it.

## Carried forward / open

**Awaiting the owner's decision:**

- **Decision 0017** (accepted 2026-10-15), the revisit of 0016 the owner
  asked for, from the lock study (`docs/phase3-data/step10-lock/`): one
  lock and the ticket lock stay (an MCS lock was measured: no gain, 52
  cycles more uncontended); the timer is to be written after the unlock
  (servers with their own contexts then scale 3.04 times on 4 CPUs, against
  0.85), once its argument is written and every self-test passes with it.
  Done after this record's first version: the default since 2026-10-15,
  measured at 2.91 times on 4 CPUs (old order 0.84), with the uncontended
  cost unchanged; it found and fixed a latent stack bug (*Found along the
  way*). 0017 also records where 0016's text and the code differ.
- **The exit benchmark's two misses** (above): the do-nothing call's 99th
  percentile and the contended line's median. Whether the criterion is met
  is the owner's call; the figures are as measured.
- **Bare metal** is still not available. Every real-CPU figure in this
  phase is under KVM: VM 112 (i5-14400) until 2026-10-14, the P53
  (i7-9850H) since.
- **Passive IPC does not scale across CPUs** under one lock: 4 pairs make
  0.57 of one pair's throughput. 0017's trigger is what reopens it.

**Settled during step 10** (each was open in the draft of this record):
the run queues (built), `Yield` (built), the two clusters (explained and
fixed), `fuzzchild` (now reaches calls 54 and 72 too), and the stale text
in the plan, `docs/abi.md`, `syscall.S`, the data README and
`.github/copilot-instructions.md`.

**Open, smaller:**

- **Guard pages** exist only under the per-CPU kernel stacks, not under the
  double-fault, NMI and machine-check stacks.
- **`sysfuzz` was not extended to every call**, as the plan's *Testing*
  table says. It is Phase 2's. Every call is reached by `user/fuzzchild`
  (inside the growth test) and by the host's `fuzz-abi` instead.
- **The build VM has no key for the P53**, so `P53=1 make …` does not run
  from it. Boots on the P53 were run from the workstation.

**Not done in Phase 3, from the Progress entries:**

- Every CPU wakes for every deadline; a single timekeeper would spare the
  others.
- The first CPU's timer calibration is used for all CPUs (not checked).
- A server with its own context is about ten times slower than the passive
  cases under KVM: each context change sets the timer. A TSC-deadline
  timer or bare metal would tell how much is real. For Phase 13, with the
  double register copy on the way through.
- `run.c`'s scheduling glue is tested by boot only. Broken kernels (no
  charging, no timer arming, no PCID stale mark, a page-table leak) were
  each tried once by hand, not kept as mutations.
- Fault injection does not cover: `task_reap`, suspend or abort as kills;
  death by fault; two capabilities in one message; absent or occupied
  receive slots; IPC buffer words when a sender dies; passive servers in the
  saved and transfer scenarios; a `ReplyRecv` answering a dead caller. The
  check for a capability deleted before its Call starts has never run.
- The IPC buffer copy is a loop of 64-bit words.

**To Phase 4 and later:** revoke and 0011's batched copy (`fork`, Phase 4);
page-fault IPC to a memory manager, and donated memory (seL4-style untyped),
reconsidered with it (Phase 4); grants and bulk transfer (0006, Phase 5);
signals on fault IPC (Phase 7); stable ABI guarantees (Phase 8); IRQ, I/O
and MMIO capabilities (0012, Phase 10); finer-grained locking if
measurements ask for it.

## The kernel at exit

Lines in the tree as committed with this record. "Code" is non-blank,
non-comment lines (a small comment-aware counter over `/* */` and `//`).
"All" is `wc -l`. `.c` and `.h` files; `kernel/include/limine/limine.h`
(third party), the linker script, tests and fuzz harnesses are left out.

| | Code | All |
|---|---:|---:|
| `kernel/core` (the object layer) | 3 533 | 4 111 |
| `kernel/include/joist` (`core.h` and two small headers) | 399 | 958 |
| `kernel/mem` | 492 | 648 |
| `kernel/sys` | 536 | 708 |
| `kernel/elf` | 128 | 170 |
| `kernel/lib` | 79 | 92 |
| **Portable C** | **5 167** | **6 687** |
| `kernel/arch/x86_64` (C) | 2 176 | 2 804 |
| Assembly (`isr.S`, `syscall.S`) | 130 | 184 |
| **Kernel total** | **7 473** | **9 675** |

The run queues added about 310 lines of code to `kernel/core`; the lock
study's instrumentation (all of it inside `#ifdef`s, off by default) about
270 to `run.c`.

Against the tripwire: §5.1 sets ~15 000 lines of C in the core kernel,
excluding arch-specific code; portable C is 5 167 (6 687 by `wc -l`). The
plan's estimate (*Risks*, *Line count*): "near 7 000 of the 15 000-line
tripwire"; the whole kernel is 7 473 (9 675 by `wc -l`). End of Phase 2:
2 248 (counted then without saying whether comments were included).

| | |
|---|---|
| Model tests | 170 (`JT_TEST` in `model/test/*.c`, 21 files), under plain, ASan, UBSan, MSan, TSan and the small build; CBMC on 7 operations; TLA+ (`model/tla/IpcDelete.tla`) |
| Kernel host tests | 52 (`JT_TEST` in `kernel/test/*.c`), in five builds; libFuzzer on the ABI dispatcher, the ELF validator and the system-call layer |
| Boot self-tests | 71 runs in `kernel/test/selftest.sh`: 55 `run` lines, of which 44 at top level, 10 inside `for ncpu in 2 4` (20 runs) and 1 inside the seven-seed fault-injection loop (7 runs). 21 need `/dev/kvm` and say SKIPPED without it. The bench-gate check after `bench-gate` is not counted |
| Mutations | 162 in `model/test/mutations.txt` (23 of them the run queues'), 54 in `kernel/test/mutations.txt` (non-blank lines not starting with `#`) |
| User programs | 23 in `user/`: 7 from Phase 2 (`hello`, `badptr`, `ud`, `rowrite`, `spin`, `sysfuzz`, `sysrethole`), 16 from Phase 3 (`root`, `ping`, `pong`, `schedroot`, `worker`, `faultroot`, `faulter`, `cascaderoot`, `benchroot`, `ipcbench`, `smproot`, `growroot`, `fuzzchild`, `faultinj`, `fiworker`, `scaleroot`); `user/lib/` (5 headers) |
| Toolchain | clang/LLD 21.1.8, Limine 12.9.2, CBMC 6.11.0 (with `gcc` 14.2.0 as its preprocessor), TLA+ tools v1.7.4 (TLC 2.19); QEMU 10.0.13 in the build VM, 11.1.2 on the P53 (`docs/testbed.md`) |
| Machines | build VM `joist-dev2` on the P53 (8 vCPUs, KVM inside); real-CPU boots on the P53, i7-9850H, 4 vCPUs pinned to host CPUs 2-5 |

## Verification

The final full run on the tree committed with this record (2026-10-15), on
joist-dev2. Its suites were split between this session and the P53's
Claude Code, run in parallel in copies of the same tree (`diff -r` empty):

| Suite | Result |
|---|---|
| model check (plain, ASan, UBSan, MSan, TSan, small) | 6 × 170 tests, 0 failed |
| model mutate | 162 of 162 caught |
| model cbmc | 7 of 7 operations verified |
| model tla, tla-mutate | safe and large configurations hold; the unsafe one leaks, as expected |
| model fuzz, fuzz-sanity | passed |
| kernel check | `tidy` clean (22 sources), 5 × 52 host tests, 71 of 71 self-tests, the bench gate (496, 1 548, 1 092 against the same) |
| kernel mutate | 54 of 54 caught |
| kernel fuzz-abi, fuzz-elf, fuzz-syscall | passed |

The first pass of this run failed `kernel check` on two `clang-tidy`
findings in the run queues' code (an implicit widening, a narrowing
conversion). They were fixed with two casts; `kernel check`, `model check`
and `model cbmc` were run again on the fixed tree, and are the figures
above. The other suites ran on the tree before the two casts, which no
mutation's text touches.

