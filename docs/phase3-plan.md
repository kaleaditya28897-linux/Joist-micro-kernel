# Phase 3 plan: IPC and capabilities

Plan §11, Phase 3: *implement capabilities, endpoints, sync IPC with fast
path, reply capabilities, notifications, lifetime notifications,
priority/scheduling-context donation. Benchmark round-trip IPC latency in
QEMU and on real hardware; record in CI.* **Exit: IPC benchmarks stable;
fault-injected client/server tests pass; no kernel memory growth under
adversarial tests.**

This file turns that into concrete work, as `docs/phase2-plan.md` did for
Phase 2. It is a plan, not a decision: choices marked *(changeable)* can be
changed without a journal entry until later phases build on them. The one
choice big enough for the journal, how the kernel is made preemptible and
safe on more than one CPU, is decision **0016** (proposed).

**Status: proposed, 2026-10-06.** Not started.

## Inputs

What earlier phases hand to this one:

- **The model** (`model/`, Phase 1): capabilities, endpoints, synchronous IPC
  with reply capabilities and reply objects, notifications, no-senders and
  dead-name, tasks and threads, quotas, parent authority. Tested under every
  sanitizer, with property tests, mutation, libFuzzer, TLA+ and CBMC. Its
  `src/` already obeys the kernel rules (§5.1, 0003) so that it can move into
  the kernel (§8.1).
- **`docs/abi.md`**: the semantics, and five items still open (below).
- **The kernel** (`kernel/`, Phase 2): boot, traps, page tables, frame
  allocator, timer, one ring-3 task, `syscall` entry with two debug calls.
- **Carried forward** (`docs/phase2-exit.md`, `docs/phase1-exit.md`): the
  real system-call ABI; more than one task; fault IPC (0009); SMP (0003);
  keeping the kernel half the same in every address space; capability
  transfer; preemptible cascades with TLA+'s rule; revoke and the batched
  copy; `-fstack-protector-strong`; real hardware.

## Scope

| In Phase 3 | Not until later |
|---|---|
| The model's object layer as the kernel's, one source for both | — |
| The binary system-call ABI (`docs/abi.md`, *Encoding*) | stable ABI guarantees (Phase 8) |
| Many tasks and threads: address spaces, context switch | `fork`, COW, the memory manager (Phase 4) |
| Frame capabilities and a minimal `map`/`unmap` (0013's root task partitions memory) | page-fault IPC to a pager (0007, Phase 4) |
| Capability transfer in messages; the 4 KiB IPC buffer | typed bulk transfer, grants (0006, Phase 5) |
| The two-level capability table (0001 amendment, item 4) | — |
| Fixed-priority scheduler, 256 levels, scheduling contexts with budget and period, donation on `Call` | — |
| A fast path for `Call` and `ReplyRecv` | — |
| Fault IPC (0009) and thread control (0010: registers, TLS base, abort) | signals on top of it (glibc, Phase 7) |
| Preemptible cascades (task destroy, last-capability delete) | — |
| SMP bring-up, under 0016 | finer-grained locking, if measurements ask for it |
| IPC benchmarks in QEMU, recorded | real hardware: as soon as a machine is available (see *Risks*) |
| Fault-injected client/server tests; the memory-growth test | RS and DS themselves (Phase 6/7) |
| — | revoke and 0011's batched copy (needed by `fork`: Phase 4) |
| — | IRQ, I/O and MMIO capabilities (0012, Phase 10) |

## Constraints carried in

- **§5.1:** no recursion, no VLAs, no allocation after boot, bounded work per
  system call, user memory only through `copy_from_user`/`copy_to_user`, the
  15k-line tripwire (2 248 lines at the end of Phase 2).
- **0003:** per-CPU state preallocated from `JOIST_MAX_CPUS`; a documented
  lock order, asserted in debug builds; no lock held across an IPC transfer or
  a user-sized loop; reclamation through fixed per-CPU retire slots.
- **0004 and its amendment:** every object paid for from a task's quota;
  parent authority over descendants.
- **TLA+'s rule** (`model/tla/IpcDelete.tla`): a space whose task is being
  destroyed accepts no new capabilities, from the moment it is marked dead.
- **Model first.** Every semantic change is made in the model and its tests
  before the kernel relies on it, so the property tests, mutation, fuzzing and
  CBMC keep covering the code the kernel runs.

## Choices this plan makes

1. **One object layer, two builds.** `model/src` moves to `kernel/core/`
   and becomes the kernel's code; `model/` keeps its tests, fuzzer, CBMC
   and TLA+ and builds them against `kernel/core/`. What the model kept in one
   caller-provided struct stays one struct, now placed by the kernel at
   boot. *(changeable in detail; the single source is the point)*
2. **Kernel memory: fixed pools, sized at boot.** The object, thread,
   capability-space, accounting and dead-name tables are carved once from the
   frame allocator at boot, sized by compile-time capacities, and never grow
   (0003). Quotas (0004) ration them between tasks exactly as in the model.
   Capability-table leaves (choice 6) come from the same pools. Donating raw
   memory to the kernel (seL4-style untyped) is not needed for Phase 3's exit
   and is reconsidered in Phase 4 with the memory manager. *(changeable)*
3. **Concurrency and preemption: decision 0016.** In short: the kernel runs
   with interrupts off and one kernel lock; long operations (the cascades)
   stop at bounded **preemption points** and continue from the model's work
   list; SMP takes the same lock. Measured, then revisited only if the
   benchmarks need it.
4. **Address spaces.** One PML4 per task. The kernel half is shared by
   construction: the 256 upper PML4 entries point to kernel PDPTs created
   once at boot and never replaced, so a later kernel mapping appears in every
   space with nothing to copy (Phase 2's "copied once" problem). PCID is used
   if present, to keep the TLB across switches.
5. **Memory for tasks (minimal, for Phase 3 only).** At boot the root task
   gets frame capabilities for all free memory (0013). `frame_split`, `map`
   and `unmap` let it give memory to the tasks it creates. Page tables for a
   task are charged to that task's quota. No faults are resolved: a user page
   fault is fault IPC (0009) to the handler, which may map and resume.
   Phase 4 builds the memory manager on top. *(changeable)*
6. **The two-level capability table** (0001 amendment, item 4): a fixed root
   of 64 entries, each pointing to a 4 KiB leaf of 128 slots, so up to 8 192
   slots per task, O(1) lookup. Leaves are charged to the task's quota.
   *(the 65 536-slot ceiling of the amendment is reachable by a larger root
   if a measured server needs it)*
7. **Capability transfer** (`docs/abi.md`, open item 1): a `Call` or reply
   carries **up to 2 capabilities**, named by slot in the sender's space and
   received into slots the receiver named in its IPC buffer before waiting.
   Rights are masked per transfer; the receiver learns how many arrived. If
   the receiver offered no slot, or its space is dying (TLA+'s rule), the
   message is delivered without them and the count says so: the transfer
   never blocks the sender or allocates. To be settled in the model first.
8. **The system-call ABI.** Number in `rax`; arguments in `rdi`, `rsi`,
   `rdx`, `r10`, `r8`, `r9`; message registers 0–3 in `r12`–`r15` (0001
   amendment: 4 × 64 bits), so a short `Call`/`ReplyRecv` never touches
   memory; result in `rax`, badge in `rdx`. Longer messages use the
   per-thread 4 KiB IPC buffer, at an address registered by the thread. The
   two Phase 2 debug calls stay, debug builds only. Written into
   `docs/abi.md`, *Encoding*.
9. **Scheduler** (§5.6): 256 fixed priorities, a bitmap plus per-priority
   FIFO per CPU, O(1). A **scheduling context** holds a budget, a period and
   a priority; a thread runs only with one. On `Call` the client's context is
   **donated** to the server for the call and returns with the reply; a
   server thread with no context of its own runs only on donated time (§5.3:
   the server runs at the client's priority). The timer moves from a fixed
   100 Hz tick to one-shot deadlines.
10. **Fast path:** `Call` and `ReplyRecv` with only message registers, when
    the peer is waiting and nothing else needs doing, switch directly
    without the scheduler. The slow path stays the specification; the fast
    path is checked against it (same results on the same inputs, by a test
    that runs both).
11. **Fault IPC** (0009): a fault (any exception from ring 3, including page
    faults) suspends the thread and makes a `Call` to its fault handler,
    falling back to the task's, carrying the fault type, address and error
    code, and a thread control capability. The handler replies *resume* or
    *terminate*. No handler, or a dead one, ends the task. A fault while
    handling a fault for the same thread ends the task.
12. **User side:** `user/lib/` with the system-call stubs and a minimal
    runtime; a real root task (0013) that partitions memory and starts the
    test programs, loaded as Limine modules.

## Testing

| What | How |
|---|---|
| The object layer | the model's whole suite, now against `kernel/core/`: unit and property tests under every sanitizer, whole-model executor, libFuzzer, mutation, CBMC, TLA+ |
| Capability transfer, the two-level table, scheduling contexts, thread state | added to the model first, with property tests against shadows |
| System-call decoding | host tests and libFuzzer on the dispatcher, as in Phase 2 |
| Fast path | a differential test: fast and slow path on the same random inputs must agree |
| Preemption points | TLA+ (already models the cascade's order); a test that preempts at every point in turn |
| In QEMU | the boot tests and self-tests of Phase 2, plus multi-task programs: ping-pong, deferred replies, notifications, no-senders and dead-name, fault handling |
| Exit: benchmarks stable | `user/ipcbench`: round-trip `Call`/`ReplyRecv` latency in TSC cycles, same address space and cross address space, fast and slow path. Recorded to `docs/phase3-data/` with QEMU version, host CPU and command. *Stable* means: across 10 runs, the median moves less than 5% and the 99th percentile less than 20%, and a regression gate in `make check` fails at +25% |
| Exit: fault-injected client/server tests | `user/faultinj`: clients and servers that die or are killed at random points (mid-`Call`, holding a reply capability, holding a saved reply, during transfer, while signalling), with checkers that every survivor gets `E_PEER`/`E_GONE`, no-senders or dead-name exactly as `docs/abi.md` says, and nothing hangs. Seeded, many runs |
| Exit: no kernel memory growth | an adversarial root task that creates and destroys tasks, threads, endpoints and capabilities in loops and cycles, exhausts quotas and fixed tables, and fuzzes the full system-call surface. The kernel's pool counters and frame allocator are read at checkpoints: back to their starting values after every round, and no table ever over its capacity. Plus Phase 2's `sysfuzz`, extended to every call |
| Static analysis, UBSan | as Phase 2, in `make check` |

## Order of work

1. **Decision 0016**, and this plan, accepted.
2. **One object layer:** move `model/src` to `kernel/core/`; `model/` builds
   against it; it compiles freestanding. Nothing changes in behaviour.
3. **The model grows** (host only): capability transfer, the two-level
   table, scheduling contexts and donation, thread state and abort, fault
   IPC, frame capabilities and the mapping bookkeeping, preemption points in
   the cascades. Each with property tests and mutations; CBMC and TLA+
   extended where they apply.
4. **Address spaces and threads in the kernel:** shared kernel half, context
   switch, many tasks, the ABI encoding, the real system calls over
   `kernel/core/`. The root task (0013) starts two tasks that ping-pong.
5. **Scheduler and scheduling contexts**, one-shot timer, donation.
6. **Fault IPC and thread control.**
7. **Preemptible cascades** in the kernel.
8. **Fast path**, with the differential test, and **`ipcbench`**.
9. **SMP** under 0016: start the other CPUs (Limine's MP request), per-CPU
   state, the kernel lock, IPIs for remote wake-up; every test again with
   2 and 4 CPUs.
10. **Exit:** fault injection, the memory-growth test, benchmarks over 10
    runs, `-fstack-protector-strong`, the exit record.

## Risks

- **Real hardware.** §11 asks for benchmarks on real hardware too. That needs
  an x86_64 machine that can boot from USB with a serial port or a debug
  console. Until one is named, the QEMU numbers are the record, and the gap is
  stated in the exit record.
- **The fast path is where microkernels hide bugs.** Hence the differential
  test, and the slow path stays the reference.
- **Line count.** The model's core is ~2 000 lines; with the scheduler, fault
  IPC, address spaces and SMP, Phase 3 may end near 7 000 of the 15 000-line
  tripwire. Counted at every step.
- **Benchmark noise in QEMU** (TCG without KVM is slow and jittery). Measured
  under KVM where available, recorded with which one was used.
