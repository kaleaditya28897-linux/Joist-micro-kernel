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
safe on more than one CPU, is decision **0016** (accepted 2026-10-06).

**Status: accepted by the project owner, 2026-10-06**, with decision 0016.
No bare-metal machine has been named yet. Since 2026-10-06 every boot test
also runs on pvehome's VM 112 (`docs/testbed.md`): a real Intel CPU through
KVM, 4 vCPUs pinned to P-cores. It is where SMP is tested and the
benchmarks are recorded, until bare metal is available (*Risks*).

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

1. ~~**Decision 0016**, and this plan, accepted.~~ Done 2026-10-06.
2. **One object layer:** move `model/src` to `kernel/core/`; `model/` builds
   against it; it compiles freestanding. Nothing changes in behaviour.
3. ~~**The model grows**~~ (done 2026-10-09; see *Progress*) (host only): capability transfer, the two-level
   table, scheduling contexts and donation, thread state and abort, fault
   IPC, frame capabilities and the mapping bookkeeping, preemption points in
   the cascades. Each with property tests and mutations; CBMC and TLA+
   extended where they apply.
4. ~~**Address spaces and threads in the kernel**~~ (done 2026-10-10; see *Progress*)**:** shared kernel half, context
   switch, many tasks, the ABI encoding, the real system calls over
   `kernel/core/`. The root task (0013) starts two tasks that ping-pong.
5. ~~**Scheduler and scheduling contexts**~~ (done 2026-10-10; see *Progress*)**,** one-shot timer, donation.
6. ~~**Fault IPC and thread control.**~~ (done 2026-10-10; see *Progress*)
7. ~~**Preemptible cascades** in the kernel.~~ (done 2026-10-13; see *Progress*)
8. ~~**Fast path**, with the differential test, and **`ipcbench`**.~~ (done 2026-10-13; see *Progress*)
9. **SMP** under 0016: start the other CPUs (Limine's MP request), per-CPU
   state, the kernel lock, IPIs for remote wake-up; every test again with
   2 and 4 CPUs, locally (TCG, `-smp`) and on VM 112 (KVM, 4 pinned
   vCPUs), where the CPUs really run in parallel.
10. **Exit:** fault injection, the memory-growth test, benchmarks over 10
    runs, `-fstack-protector-strong`, the exit record.

## Risks

- **Real hardware.** §11 asks for benchmarks on real hardware too. That needs
  an x86_64 machine that can boot from USB with a serial port or a debug
  console. Until one is named, VM 112's numbers (real Intel CPU, KVM, pinned
  P-cores) are the record, alongside local TCG, and the gap to bare metal is
  stated in the exit record.
- **The fast path is where microkernels hide bugs.** Hence the differential
  test, and the slow path stays the reference.
- **Line count.** The model's core is ~2 000 lines; with the scheduler, fault
  IPC, address spaces and SMP, Phase 3 may end near 7 000 of the 15 000-line
  tripwire. Counted at every step.
- **Benchmark noise in QEMU** (TCG without KVM is slow and jittery). The
  record is VM 112 under KVM, pinned to P-cores; each result names the
  machine it came from.

## Progress

### Step 2: one object layer (2026-10-06)

`model/src/*.c` and `internal.h` moved to `kernel/core/` and
`model/include/joist/model.h` to `kernel/include/joist/core.h` (with `git
mv`, so their history follows). `model/` builds its tests, fuzzer, CBMC
harness and mutations against `../kernel/core`; the mutation and
fuzz-sanity scripts copy both trees into their scratch area.

The core compiles freestanding with §13's flags; its only outside symbol is
`memset`, which `kernel/lib/string.c` already provides. On the host,
`internal.h` keeps `<string.h>` and `snprintf`; freestanding, it declares
`memset` and replaces the invariant checkers' formatted messages with their
bare format text (bounded copy), so a debug kernel can still run the checks.
It is linked into the kernel image (unused so far), so `clang-tidy` and
`scan-build` cover it in `make check`.

`clang-tidy` on the core, its first run, found three things:

| Finding | What it was | Done |
|---|---|---|
| `clang-analyzer-security.ArrayBound` in `q_remove` | the analyzer's path has a queued thread whose `ep` is 0, so `obj[ep - 1]` indexes out of bounds. The invariants exclude that state (`joist__check_ipc`), but the kernel shouldn't depend on an invariant for a memory access when the caller knows the answer | `q_remove` now takes the object index from its caller: `joist__q_pop` passes the object it pops from, the other two callers check `ep != 0` first |
| `bugprone-branch-clone` ×2 in `full_rights` | endpoints and notifications return the same rights, and so do tasks, threads and reply objects (CONTROL is the SEND bit) | cases merged, with the reason in a comment |
| `insecureAPI.DeprecatedOrUnsafeBufferHandling` ×21 | every `memset`, for want of Annex K's `memset_s`, which doesn't exist freestanding | check disabled in `/.clang-tidy`, reason recorded there |

Checked after the move (2026-10-06): `make check` in `model/` (80 tests
under plain, ASan, UBSan, MSan, TSan and the small build); `make mutate`,
46 of 46 caught; `make cbmc`, all five operations verified (op0 188 s, op1
112 s, op2 202 s, op4 181 s, op5 68 s); `make fuzz` for 60 s, clean;
`make fuzz-sanity` with one mutation, to show the changed script still
copies the right trees (caught in 0 s). In `kernel/`, `make check`: 9
sources freestanding, 18 clean under `clang-tidy`, `scan-build` clean, boots
and self-tests as before. No behaviour changed.

### Step 3, part 1: the two-level capability table (2026-10-06)

The table of the 0001 amendment (item 4), in `kernel/core/` and its
tests. A space has a root of `JOIST_CNODE_ROOT` entries naming leaves of
`JOIST_CNODE_LEAF` slots (kernel: 64 × 128 = 8 192 slots). Leaf 0 is the
space's own `slot[]` array, so every space has it and nothing about slots
0–127 changed; leaves 1 and up come from a fixed pool (`JOIST_MAX_LEAVES`),
added by `joist_cspace_grow` and removed, empty only, by
`joist_cspace_shrink`, each one quota unit (I3), counted in the space's
`charged` like an object. A slot in an absent leaf is `E_RANGE`, so no
lookup allocates, which is what capability transfer (next) needs. Every
access in the core now goes through `joist__slot` (O(1)); walks over a
space go leaf by leaf, skipping absent ones. Task destruction empties every
leaf, then frees the leaves into the space's quota, which goes to the
parent as before. The invariant checker checks the table itself: every pool
leaf in use is named by exactly the space and index it records, only live
spaces have leaves, and charges include leaves.

Tests: the host build uses 2 leaves of 64 (`model/Makefile`), so the 80
existing tests run unchanged in leaf 0, and new ones exercise leaf 1:
`test/test_cnode.c` (7 unit tests: absent leaves, grow/use/shrink, errors,
quota and pool exhaustion, destruction returning leaves, reachability
through a capability held only in leaf 1, and the checker catching broken
leaves); the whole-model executor gained `grow` and `shrink` and picks
slots across the whole table. Six mutations added. CBMC now runs with two
1-slot leaves, so slot 1 is in a pool leaf in every proof, and proves grow
and shrink (operations 7 and 8).

**CBMC found an overflow** in the first proof of `shrink`: a fixture space
whose quota was `UINT32_MAX` held a leaf, and the refund wrapped the 32-bit
quota to 0, losing a unit from the conservation sum (I3). In the kernel the
total is the boot budget, itself 32 bits, so it can't happen there; but
fixtures (`joist_cspace_create`) each added their quota to a 64-bit budget
unchecked, so with fixtures any refund could wrap, including the one in
`cap_delete`, whose CBMC proof is out of memory and so never saw it. Fixed
at the source: `joist_cspace_create` refuses a quota that would take the
budget past `UINT32_MAX`, and the invariant checker now requires the budget
to fit 32 bits, so every quota, a share of it, does too. A unit test and a
mutation (`budget-unbounded`) cover it; the `shrink` proof then passes.
One existing mutation (`I1 destroy leaks`) had to be re-aimed at the new
leaf-by-leaf destruction loop, since its old target text was gone.

Checked (2026-10-06): `make check` in `model/`, 88 tests under all six
builds; `make mutate`, 53 of 53 caught (7 new); `make cbmc`, all 7
operations verified with the two-leaf sizes (op0 261 s, op1 186 s, op2
304 s, op4 230 s, op5 111 s, grow 74 s, shrink 93 s); `make check` in
`kernel/`, all passing.

### Step 3, part 2: capability transfer in messages (2026-10-06)

Choice 7 of this plan, now in the model and closing `docs/abi.md`'s open
item 1. `struct joist_msg` gained `ncaps` and, per position (2), a slot and
a rights mask; a message initialised to zero carries none, so nothing else
changed. A thread names its receiving slots with `joist_recv_slots` (the
kernel will read them from the IPC buffer). Capabilities are checked at
send (`Call`, `Reply`, `ReplyRecv`, `reply_saved`: in range, present,
GRANT), so a bad message fails with no effect, keeping a reply capability
or a reply object's binding. They are copied at delivery, through
`joist__cap_grant` (live spaces, GRANT, empty destination, masked rights,
badge and no-senders accounting), and anything that can't be placed then is
skipped, never an error: the receiver's message says per position where
each landed or `JOIST_SLOT_NONE`. A transfer never blocks, never allocates
(an absent leaf is not grown), and never goes into a dead space (TLA+'s
rule, which the kernel's preemptible destruction will keep with a dying
flag).

Tests: `test/test_xfer.c`, 11 unit tests (to a waiting and to a queued
receiver, replies and deferred replies carrying capabilities back, send-time
errors with no effect, positional skipping, a capability deleted while its
Call waits, a server's own badged capability coming back and no-senders
firing only after both copies go, transfer into grown and absent leaves,
`recv_slots` errors, and no default to slot 0). The whole-model executor
attaches capabilities to half of its Calls and replies and has a
`recv_slots` operation; its full run must move and skip capabilities (1 735
arrived, 966 skipped, every invariant checked after every step). Seven
mutations, one per rule, all caught; `mutate.sh` gained `MUTATE_ONLY` to
run a subset.

Checked (2026-10-08): `make check` in `model/`, 99 tests under all six
builds; `make mutate`, 60 of 60 caught; `make cbmc`, 7 of 7 verified (the
grant operation, now also each delivery's copy, included); `make fuzz` for
120 s, clean; `make check` in `kernel/`, passing. CBMC does not cover the
delivery loop itself (the harness has no threads); it is two calls of the
proven grant, and the unit tests, executor and mutations cover the rest.

### Step 3, part 3: scheduling contexts and donation (2026-10-08)

Choice 9's semantics in the model (`kernel/core/sched.c`); the run queues,
budgets and the one-shot timer are the kernel's (step 5). A new object
type, the scheduling context (priority, budget, period), made by
`joist_sc_create` within the space's **mcp** (the highest priority it may
give; the root's is 255, a new task inherits its creator's, and
`joist_task_set_mcp` sets a task's to at most the caller's own), and bound
to a thread with `joist_sc_bind`. A thread has `sc` (its own) and `run_sc`
(what it runs on); without its own it is passive.

Donation: a Call delivered to a passive receiver lends the caller's
`run_sc`; `sc_to` on the lender and `sc_from` on the borrower link them, so
a borrower calling another passive server makes a chain. The context comes
back to the caller whenever it wakes (reply, timeout, peer death), pulled
along the chain from wherever it is, and from a server when it receives or
waits again or dies, so a deferred reply never strands a client's time.
Deleting a context unwinds its loan. Chosen over seL4's rule of tying the
loan to the reply object: here a saved reply hands the time back at once,
so a server deferring many replies holds no one's time while it waits,
which suits the Hurd's deferred-reply servers (`proc_wait`, `select`).

**I11** (new, in `joist_check_invariants`): no two threads run on one
context; a context and its owner name each other; every loan is recorded at
both ends, ends at a holder, never loops, and its lender is blocked waiting
for a reply; free threads and non-context objects carry no scheduling
state.

Tests: `test/test_sched.c`, 14 unit tests (creation limits and mcp, bind
and unbind, lending to a passive server and not to an active one, at
delivery and at receive, return on reply, timeout, server death and
deferral, a two-deep chain unwinding reply by reply, a timeout pulling the
time back through the chain, a context deleted while lent, unbinding
refused while lent, a dying client, and the checker catching bad links).
The executor creates, binds and unbinds contexts and sets mcps, and starts
each sequence with one active and two passive threads; its full run must
lend and chain (15 752 steps with a loan standing, 383 with a chain, I11
checked after each).

Checked (2026-10-08): `make check` in `model/`, 113 tests under all six
builds; `make mutate`, 70 of 70 caught (10 new: four by I11 in the random
run, six by the unit tests); `make cbmc`, 7 of 7 verified with the larger
object and thread records; `make fuzz` 120 s, clean; `make check` in
`kernel/`, passing (19 sources clean under `clang-tidy`). CBMC's harness
has no IPC, so donation is covered by the unit tests, the executor's I11
checks and the mutations.

### Step 3, part 4: thread control and faults (2026-10-08)

Decisions 0009 and 0010 in the model (`kernel/core/fault.c`, with the
fault call in `ipc.c`). A thread control capability reads and writes the
thread's register record (generic in the model: pc, sp, TLS base, 8
registers), aborts its blocking operation (`E_ABORTED`, a new result code:
glibc's `EINTR`), suspends it (a new thread state) and resumes it. A
thread's fault handler, or its task's, is a slot in its own space, looked
up when the fault happens (seL4's way: no hidden kernel-held capability).
A fault is a Call on it carrying kind, address, error code and a thread
control capability, lending the faulting thread's context; the handler's
reply resumes (`r[0] = 0`) or destroys the thread.

Two choices of this step, both changeable: **a fault call that ends without
the handler's reply leaves the thread suspended** (0009 terminates the task
only when there is no handler at fault time; after delivery, destroying
from inside a wake-up could recurse, and a stopped thread is what a parent
or debugger wants to see), and **with no handler the root task loses only
the faulting thread**, since it can't be destroyed (0013); the kernel will
also report it. A new check in the IPC invariants: a thread marked faulting
is in its fault call (queued, or waiting for the reply).

Tests: `test/test_fault.c`, 12 unit tests (registers, abort of a receive
and of a reply wait, suspend and resume, a fault resumed with rewritten
registers, a fault terminated, no handler ending the task, the task's
handler as fallback, a handler dying and an abort leaving the thread
suspended, a queued fault carrying the capability, the fault charged to the
faulting thread's context, a dead handler endpoint, and the root task). The
executor reads and writes registers (checking they read back), aborts,
suspends, resumes, sets handlers and raises faults; the root task's fault
handler is its endpoint, and replies to faulting callers resume half the
time, checked for the right outcome. Its full run must deliver and fail
faults (715 to a handler, 2 122 ending a task or thread). Faults count as
cascading operations in the work bounds (I9), since one can destroy a task.

Checked (2026-10-08): `make check` in `model/`, 125 tests under all six
builds; `make mutate`, 80 of 80 caught (10 new); `make cbmc`, 7 of 7
verified; `make fuzz` 120 s, clean; `make check` in `kernel/`, passing
(20 sources clean under `clang-tidy`).

### Build and test environment: `joist-dev` (2026-10-09)

The workstation moved to Alpine (musl), where clang 21's sanitizer
runtimes, `scan-build` and CBMC aren't available. Builds and tests moved to
VM 113 on pve1 (`docs/testbed.md`), with the same tool versions (clang
21.1.8, Limine 12.9.2, CBMC 6.11.0, TLA+ v1.7.4); `tools/dev/remote.sh`
runs a command there on the current working tree. `make check` in `model/`
and `kernel/` passed there in full before any further work. `mutate.sh`
now runs mutations in parallel (80 in 13 minutes on 12 vCPUs). From here,
each part of step 3 runs its new mutations; the whole suite (with CBMC,
fuzzing and the kernel check) runs once at the end of the step, as agreed
with the project owner.

### Step 3, part 5: frames and mappings (2026-10-09)

Plan choice 5 in the model (`kernel/core/map.c`): frame objects (a page
range; the kernel creates them for free memory at boot, in the root's
space, 0013), `joist_frame_split` (only unshared and unmapped frames), and
`joist_map`/`joist_unmap` of whole frames into a task's user half,
readable plus writable or executable, never both. Mapping writable needs
the frame capability's WRITE right (the RECV bit), so read-only frame
capabilities exist. Each mapping costs the task one quota unit, standing
for its page tables; deleting a frame's last capability unmaps it
everywhere, and a dying task's mappings go before its quota passes to its
parent. Aliases are allowed (one frame in several tasks, or twice in one);
W^X is per mapping. New invariant **I12** (frames disjoint and non-empty;
mappings of whole live frames, into live spaces, in the user half, W^X,
non-overlapping per space, and charged).

Tests: `test/test_map.c`, 8 unit tests (boot frames and their overlap
check, splitting and its refusals, map and unmap with quota, the
refusals: W^X, WRITE, the user half, types; quota exhaustion, a freed
frame unmapped in two tasks, task destruction, and the checker catching
overlaps). The executor splits, maps and unmaps (from the root half the
time, unmapping where mappings are), with two boot frames in its starting
world; its full run must keep mappings standing (44 115 steps). Ten
mutations, all caught (seven by I12 or I3 in the random run). Checked per
the agreed routine: `make check` in `model/` (all six builds), the new
mutations, and the kernel's freestanding compile and `clang-tidy`.

### Step 3, part 6: preemptible destruction (2026-10-09)

Decision 0016's preemption points in the model (`kernel/core/task.c`),
closing `docs/abi.md`'s open item 2. Destruction is now three parts:
**begin** (bounded: the space becomes `dying`, which every entry point
treats as dead through `joist__usable`, so nothing can be put into it,
TLA+'s rule; its task object dies; its threads are destroyed; it goes on
the work list), **sweep** (one slot per unit of work, from the space's own
cursor `sweep`, skipping absent leaves; a task made unreachable begins its
own destruction at once and is pushed on top, so the newest is swept first
and the older resumes later), and **finish** (bounded: leaves, mappings,
children, quota and charges to the parent). `k->preempt` limits the slots
one operation sweeps (0: no limit, the old behaviour); `joist_continue`
resumes. A task made unreachable begins its destruction in the same
operation, even when its sweep waits, so a doomed task never has a window
to receive a capability.

New checks: the work list holds exactly the dying spaces, once each; a
dying space has no task object and owns no threads; and **no capability
sits in a slot its sweep has passed**, TLA+'s SpaceEmptied as a C check.
The checker no longer forbids pending work.

Tests: `test/test_preempt.c`, 6 unit tests (destruction in single-slot
steps with every intermediate state checked, a dying space refusing every
operation and every way in for a capability, a cascade swept newest first
with the older resuming after it, an unreachable task dying at once, a
fault with no handler destroying in steps, and the checker seeing the
leak). The executor sets a random limit of 1 to 4 slots on a third of its
operations and resumes with a `continue` operation, so everything else runs
against half-done destructions: 45 325 steps with one pending, 6 462 with
two or more, every invariant checked after each. Eight mutations, all
caught (five by the invariants in the random run).

With this, step 3's model work is done: everything the plan put in the
model for Phase 3 (the table, transfer, scheduling contexts and donation,
thread state and faults, frames and mappings, preemption points) is in
`kernel/core/` and tested. Revoke and 0011's batched copy are Phase 4's.

### Step 3: the full run (2026-10-09)

The whole suite, run once at the end of the step as agreed, on joist-dev.
Its first pass found three things, all in the test machinery rather than
the core: five mutations had gone stale (their target text was rewritten
by part 6, or by the change to `joist__usable`; one of them, also listed
in `fuzz-sanity`, stopped that run); CBMC failed every operation in seconds
on an unwinding assertion, because its tiny table sizes didn't include the
new mapping table (64 entries against `--unwind 4`); and the kernel check,
filtered too tightly, stopped without saying why (run alone it passed in
full). The mutations were re-aimed at the new code, CBMC got
`JOIST_MAX_MAPPINGS=1`, and the second pass kept the kernel log.

| Check | Result |
|---|---|
| `make check` (`model/`) | 139 tests under plain, ASan, UBSan, MSan, TSan and the small build |
| `make mutate` | 98 of 98 caught (16 minutes, 12 at a time) |
| `make cbmc` | 7 of 7 operations verified (op0 430 s, op1 244 s, op2 309 s, op4 350 s, op5 267 s, grow 129 s, shrink 125 s) |
| `make tla`, `make tla-mutate` | both configurations hold (safe: 1 460 distinct states; large: 143 000); the unsafe one leaks as expected; 4 of 4 TLA+ mutations caught |
| `make fuzz` (300 s) | clean |
| `make fuzz-sanity` | 7 of 7 injected bugs crash the fuzzer from the seed corpus within a second; from an empty corpus 6 of 7 within 120 s (`I6 reply reusable` not, which is reported only, as before) |
| `make check` (`kernel/`) | passing: 25 host tests in five builds, 12 sources freestanding, 21 clean under `clang-tidy`, `scan-build` clean, BIOS and UEFI boots, all self-tests |

Step 3 is done.

### Step 4, part 1: memory plumbing for many address spaces (2026-10-09)

Host-tested pieces the kernel needs before it runs more than one task:

- `vm_unmap` (`kernel/mem/vm.c`): removes 4 KiB mappings, all or nothing,
  and returns page tables of the user half that become empty, bottom up,
  through a new `put_frame` callback (the shared upper half's never).
  `vm_destroy` returns every table of a space. `vm_prefill_upper` gives
  every upper-half PML4 entry a table once at boot, and `vm_share_upper`
  copies those entries into a new space, so the kernel half is shared for
  good (plan choice 4: no copying when the kernel maps something later).
  Tests: unmap with exact reclamation, refusals that change nothing, the
  upper half never freed, destroy returning every table, the shared half
  seeing a later mapping, and a property test of random maps and unmaps
  across 2 MiB and 1 GiB boundaries where the tables in use must equal,
  after every step, exactly the PML4 plus one table per occupied region at
  each level (4 942 maps, 3 870 unmaps).
- The core (`kernel/core/`): **a mapping lies within one aligned 512-page
  span** (one page table), so it needs at most one table per level and the
  kernel's page-table pool is sized from `JOIST_MAX_MAPPINGS` and
  `JOIST_MAX_CSPACES` alone, never failing where the model succeeds;
  `JOIST_SLOT_SELF` maps into the caller's own space (the root task has no
  task object, 0013); the register record is the full user state (pc, sp,
  flags, TLS base, 16 general registers), so 0010's register access is the
  thread itself; and **hooks** (`struct joist_hooks`) through which the
  core tells the kernel what to carry out: a space's tables to build or
  free, a mapping to make or remove. The core decides, the kernel executes,
  and the hooks can't fail.
- The executor installs recording hooks that keep a shadow of every
  space's page tables, and after every step the shadow must equal the
  core's record, so a missing, extra or wrong hook call anywhere fails the
  run (preempted destructions included). Four mutations (a hook left out
  in three places, the span check removed), all caught.

The remote runner (`tools/dev/remote.sh`) had a stale-build hazard: `rsync
-a` kept the workstation's modification times, so a file edited while an
earlier remote build ran arrived older than that build's output, and make
skipped it (seen once: a kernel test binary one test short). It now copies
by content and gives copied files the time of the copy.

### Step 4, part 2: giving a task capabilities (2026-10-09)

A task starts with an empty capability space, and nothing so far let its
creator put anything there (the model's tests used an internal stand-in).
`joist_cap_give` copies a capability with GRANT into a slot of a task named
by a task capability with CONTROL, rights masked, badge kept: how the root
task (0013) hands the tasks it starts their endpoints. One unit test (the
copy and every refusal), an executor operation (373 successes in the full
run), and two mutations (the mask ignored, the CONTROL check bypassed), both
caught.

### Step 4, part 3: the system-call ABI (2026-10-09)

`kernel/sys/abi.c`: the dispatcher from a thread's saved registers to the
core (encoding in `docs/abi.md`, *Encoding*): 45 calls, IPC with message
registers in R12-R15 and capabilities packed in R9, and results written
back into the record; a blocking call's outputs are written by
`abi_complete` when the thread next runs. Phase 2's `debug_write`, `exit`
and unknown-number behaviour are kept (Phase 2's dispatcher handles them),
so the existing self-tests and `sysfuzz` still hold. Host tests
(`test/test_abi.c`, 8: an unknown call changing nothing else, a Call /
Recv / Reply round trip through registers, capabilities through R9, wide
slot arguments refused, spawn starting at pc and sp, exit from a task and
from the root, `cap_read` of a frame, Wait completing on a Signal) and a
libFuzzer harness over the whole ABI (`make fuzz-abi`: sequences of calls
from several threads, preempted destruction included, every invariant
after every call; 7 980 inputs in 2 minutes, clean). `clang-tidy` found an
arbitrary user value cast to the object-type enum; it is now refused as no
type.

### Step 4, part 4: the kernel runs the core (2026-10-09)

`kernel/arch/x86_64/run.c` replaces Phase 2's single root task
(`user.c`). One `struct joist_kernel` in BSS, sized for the kernel (1 024
objects, 512 mappings: `KSIZES` in `kernel/Makefile`). A page-table pool
reserved at boot, `JOIST_MAX_CSPACES` + 3 × `JOIST_MAX_MAPPINGS` frames
(+8), from which the core's hooks build each space's tables (sharing the
kernel half, which `paging_init` now prefills) and to which `vm_unmap`
returns them. One kernel stack: `syscall` now builds the same frame as an
interrupt from user mode and joins the common path (`syscall.S`, `isr.S`);
every entry from user mode saves the thread's registers into its record,
and every exit builds a frame from the record of the thread that runs next
and enters it with `iretq` (`resume_user`), after checking its pc is in the
user half (which keeps Phase 2's `sysret` guard). Round robin for now, on
blocking and on every tick; an idle loop that reports a system where no
thread can ever run again instead of hanging. Faults go to the core
(`joist_fault`); a root-task fault with no handler is reported and ends the
run as in Phase 2.

The root task is loaded through the core: each page of its image and stack
is a frame (`joist_boot_frame`) in its leaf 1, mapped into its own space
(`JOIST_SLOT_SELF`), so the core's record of its address space is
complete. It is given a memory pool of four 512-page frames (8 MiB, carved
contiguously from the top of the largest usable range with the new
`frame_take_range`) and a frame per further boot module, and starts with
registers saying where they are (`run.h`).

Checked: the kernel boots under BIOS and UEFI on the new path, all 16
self-tests pass on it (the million-call `sysfuzz` with every register
preserved, the faults, the `sysret` hole now caught by the general user-half
check), and `make check` in `kernel/` passes (`clang-tidy` asked for two
64-bit products in a boot message).

### Step 4, part 5: the root task starts two tasks that ping-pong (2026-10-10)

The step's goal, met. `user/lib/joist.h` is the system calls from user mode
(one raw call with every ABI register, and wrappers; its numbers are checked
against the kernel's at compile time by `kernel/test/test_abi_user.c`).
`user/root` is the root task of 0013 as far as this step needs it: the
kernel hands it its memory pool and one frame per boot module (in the C
calling convention's argument registers), and it loads each module as a
task: maps the module into itself, validates it with the kernel's ELF
validator built for user mode, creates the task, builds its memory a page
at a time (a page split off the pool, mapped here to be zeroed and filled,
then mapped into the task with the segment's permissions), gives it its
capabilities with `cap_give`, and starts its thread with `thread_spawn`.
`user/pong` receives on its endpoint and answers each message plus one;
`user/ping` calls it `PING_ROUNDS` times through a badged send capability,
checks every answer, and reports to the root, which exits.

```
joist: root task: 8 MiB of memory, 2 module(s); entering it at 0x0000000000400000
root: pong and ping started
ping: 1000 round trips ok
root: ping reported 1000 round trips
joist: root task exited with code 0 (timer ticks so far: 24)
```

It ran the first time it booted. Three boot self-tests were added
(`kernel/test/selftest.sh`, now 19): `pingpong`, `pingpong-long` (200 000
round trips, 594 timer ticks, so IPC is preempted some 570 times on the
way) and `ubsan-pingpong` (the UBSan kernel). The ISO rule takes
`MODULES="pong ping"` and writes them into the image and `limine.conf`.
`make check` in `kernel/` passes: 41 host tests in five builds, `clang-tidy`
and `scan-build` clean, both boots, all 19 self-tests.

### Step 4: the full run (2026-10-10)

The whole suite at the end of the step, on joist-dev, and the step's goal
on VM 112. The first pass found two things, both in test machinery: CBMC
failed `task_create` because its arbitrary starting state now includes the
hook function pointers, which the operation calls (the harness now clears
them: the host model has none); and the kernel's mutation script copied
only `kernel/`, while a host test now includes `user/lib/joist.h`, so no
mutant built (it copies both now). Six kernel mutations were added for
this step's code, none of which had any.

| Check | Result |
|---|---|
| `make check` (`model/`) | 142 tests under all six builds |
| `make mutate` (`model/`) | 104 of 104 caught |
| `make cbmc` | 7 of 7 operations verified (op0 420 s, op1 297 s, op2 431 s, op4 369 s, op5 198 s after the fix, grow 110 s, shrink 155 s) |
| `make tla`, `make tla-mutate` | hold; the unsafe configuration leaks as expected; all TLA+ mutations caught |
| `make fuzz` (`model/`, 300 s) | clean |
| `make fuzz-sanity` | 7 of 7 from the seed corpus (from an empty corpus 6 of 7 within 120 s, reported only) |
| `make check` (`kernel/`) | passing: 41 host tests in five builds, freestanding, `clang-tidy`, `scan-build`, BIOS and UEFI boots, 19 self-tests |
| `make mutate` (`kernel/`) | 36 of 36 caught (6 new) |
| `make fuzz-abi`, `fuzz-elf`, `fuzz-syscall` (180 s each) | clean: 12 079, 57 million and 71 million inputs |
| VM 112 (real Intel CPU, KVM) | 200 000 ping-pong round trips under SeaBIOS and OVMF, correct, in about 13 to 15 timer ticks (`docs/phase3-data/README.md`) |

Step 4 is done.

### Step 5, part 1: the scheduler's rules (2026-10-10)

In the core (`kernel/core/sched.c`), host-tested: `joist_sched_pick` (the
thread of highest priority that can run, and among equals the one that has
waited longest; a thread can run if it is running, runs on a context, and
that context has budget left or is due a new period), `joist_sched_charge`
(time used is charged to the context a thread runs on, so a server on
donated time spends its client's budget; using a budget up puts the thread
behind its equals), and `joist_sched_next_event` (the running thread's
budget end, a blocked thread's deadline, or the next period of a thread
waiting for one: what the one-shot timer waits for). A context's budget is
whole again once per period, the period starting when the context is first
used after the last one ended. A woken thread goes behind its equals.

**A change of order from the plan (choice 9):** these are bounded scans of
the thread table, not yet the bitmap and per-priority queues. The scan is
the specification; the O(1) queues come with the per-CPU run queues of step
9, checked against it.

Tests: `model/test/test_pick.c`, 6 unit tests (priority and passive
threads, equals taking turns, a budget enforced per period, a server
running on its client's budget, the next event, a woken thread behind its
equals) and a simulation over 60 random sets of threads and contexts, half
lightly loaded and half overcommitted: at each of 22 560 decisions the
choice equals an independent reading of the rules, no context ever uses
more than its budget in a period, and something is always due later (1 440
idle stretches, 13 847 periods begun). Eight mutations, all caught.
`make check` in `model/`: 149 tests, six builds.

### Step 5, part 2: the kernel runs on it (2026-10-10)

`kernel/arch/x86_64/run.c` no longer chooses: every exit to user mode asks
`joist_sched_pick`. The round robin of step 4 is gone.

- **Time** is the TSC in nanoseconds (`apic_clock_ns`), its rate measured
  against the PIT in the same window that calibrates the LAPIC timer. On
  every kernel entry the core's clock is set and the thread that ran is
  charged for the time since it was entered.
- **The timer is one-shot.** The 100 Hz tick serves the boot and stops when
  the root task starts; from then on, before every exit, the timer is set
  for `joist_sched_next_event` (the running thread's budget end, a
  deadline, a new period), or switched off if there is none. Idle sleeps
  until that event; if there is none and nothing can run, the kernel says
  so and stops, as before.
- **The root task** is given a scheduling context of its own in slot 0
  (`RUN_ROOT_SC`: priority 128, 10 ms of every 10 ms), since nothing runs
  without one.
- **User side:** `j_sc_create`, `j_sc_bind`, `j_sc_unbind`; the module
  loader moved out of the root task into `user/lib/loader.h`, for the test
  root tasks of part 3 to share.
- **Starting a passive server.** Pong has no context of its own: it runs on
  what each call lends it. The first run with the scheduler stopped with
  "no thread can ever run again": pong could never reach its first `Recv`.
  The root now lends it a context of priority 200, above its own; pong runs
  at once until it blocks, and the root takes the context back
  (`docs/abi.md`, *Scheduling*). One CPU makes the priority do the
  sequencing; step 9 needs a handshake.

Not charged to anyone yet: the kernel's own time in a call (a thread is
charged from its entry to user mode to its next kernel entry). It is small
against a budget and bounded per call; noted here so the benchmark step
can measure it and decide.

Check: `make check` in `kernel/` passes, the 19 self-tests with it (the
three ping-pong runs on donated time: 200 000 round trips with pong never
holding a context of its own). In the ping-pong runs no timer interrupt
happens at all: a budget that runs out is noticed at the next system call,
microseconds away, and with a budget equal to the period the next period is
then already due. `spin`, which makes no calls, is interrupted at each
budget end (313 interrupts in its run).

### Step 5, part 3: the scheduler seen from user mode (2026-10-10)

`user/schedroot` is a root task that loads one program, `user/worker`, as
nine tasks, and shares a page with each (`user/lib/work.h`): the root writes
the worker's role there and reads its count, which moves only while the
worker runs. Every count comes from one loop, so counts compare as time and
no clock is needed. Workers below the root's priority run only while it
sleeps (a `Recv` with a timeout on an endpoint nobody sends to). What it
checks, as two new boot self-tests (`sched`, and `ubsan-sched` on the UBSan
kernel; 21 in all):

| Test | What must hold | Measured (QEMU on joist-dev, three runs) |
|---|---|---|
| Priorities | a spinner at 50 doesn't advance while the root (128) is busy for longer than its budget; it does while the root sleeps; the root's timeout takes the CPU back from it though it never calls the kernel; a spinner at 200 runs the moment it is given a context, and the root resumes only when its 2 ms are spent; no worker advances once its context is taken away | all hold |
| A budget | priority 60 with 2 ms in every 10, over a spinner at 50: the first gets about a fifth, the second the rest (bounds 8-40 %) | 20 %, 20 %, 20 % |
| Equals | two spinners at 50 share about evenly (bounds 30-70 %) | 50 %, 50 %, 50 % |
| Donation | a passive server does nothing until called, and nothing while only a spinner at 50 runs; then, for a client at 60 with 2 ms in every 10, it does the work above the spinner and out of the client's budget (bounds 8-40 %) | 557, 536, 538 calls served; 14 % each time |

The donation share is 14 % and not 20 % because a call costs more than its
work: with 20 000 counts of work per request, about a third of what the
budget is charged is the way into and out of the kernel (under emulation;
`resumed_at` is taken before the exit path, so the exit is charged to the
thread). With ten times the work per request (`-DSERVE_COUNTS=200000u`) the
same test measures 20 %. The bounds are wide on purpose: an emulator on a
busy host keeps uneven time, and what must be told apart is a fifth from
all or nothing.

**Does the test notice a broken kernel?** `run.c`'s part isn't reachable
from the host tests, so three kernels broken by hand were booted against
it, once: no charging on entry; the timer never armed before an exit; idle
arming the timer for the far future instead of the next event. Each hangs
in the first test and fails by timeout. (The rules themselves are the
core's, under the eight `pick-*` mutations.)

`make check` in `kernel/` passes with the 21 self-tests.

### Step 5, part 4: time in the kernel is charged; found on the real CPU (2026-10-10)

The scheduler test was booted on VM 112 (KVM, i5-14400) before the
end-of-step run. It passed, but its donation figure was 25 % where the
emulator gave 14 %, and following that up found two things.

**1. A hole in budget enforcement (fixed).** Part 2 charged a thread from
its entry to user mode to its next kernel entry, so the kernel's own time
was nobody's. Under emulation that hides: user-mode work is slow and
dominates. On the real CPU a kernel entry and exit (two timer writes that
leave the guest, an address-space switch) cost about as much as a short
request's work, and a thread doing little but system calls could use
several times its budget of CPU at the expense of lower priorities. Now
(`run.c`, `enter`) a thread is charged from the kernel entry that led to
its being resumed: the kernel's time between an entry and the exit that
follows is paid by the thread that exit resumes (`docs/abi.md`,
*Scheduling*). Only idle time is free. The core is unchanged: this is
what the kernel passes to `joist_sched_charge`.

A test for it was added to `schedroot` (*kernel time*): a worker that makes
the cheapest system call for ever, with 2 ms in every 10 at priority 60,
beside a spinner at 50 that must keep about four fifths of what it gets
alone (bounds 60-92 %). On VM 112 the kernel as it was fails it
(`sched: FAILED: a budget spent in system calls wasn't about a fifth of
the time`); the fixed kernel passes.

**2. A flaw in the test's measurement (fixed).** With the kernel fixed the
donation figure on VM 112 rose to 33 %. The raw counts showed why: the
spinner beside the server had kept its 79 %, and the server's counts were
more than its time could hold. The counting loop was inlined, and the
compiler's two copies (the spinner's and the server's) ran at rates a
factor of two apart on the real CPU, though alike under emulation. The loop
is now one function, never inlined (`user/lib/work.h`); the donation test
checks the spinner's share against a window of its own alone as well; a
request's work is ten times larger (200 000 counts), so the call is small
beside it; and every result prints the two counts behind it.

| Test | QEMU (TCG) on joist-dev | VM 112 (KVM), SeaBIOS |
|---|---|---|
| A budget: 2 ms in every 10 | 20 % | 20 % |
| Kernel time: a spinner beside 2 ms in every 10 of system calls keeps | 78 % | 84 % |
| Equals | 50 % | 49 % |
| Donation: the server's share on its client's 2 ms in every 10 | 18 % (78 calls) | 19 % (1 148 calls) |
| Donation: the spinner beside them keeps | 79 % | 74 % |

(The *kept* figures compare two 300 ms windows, and the VM's speed varies
by several per cent between windows; the bounds allow for it.) Part 3's
table above is from the test as it was then.

### Step 5, part 5: the timer is written only when its moment changes (2026-10-10)

The final boots on VM 112 showed the cost of part 2's "set the timer before
every exit": 200 000 ping-pong round trips took 51 budget periods (about
2.5 µs each) where step 4's kernel took about 0.7 µs. Under KVM a write to
the LAPIC timer leaves the guest, and there was one on every exit. `run.c`
now remembers the moment the timer is set for and writes it only when the
next event differs. Since a thread is charged from its kernel entry, a
budget's end is the same moment from call to call while one context runs,
through a call to a server on donated time too, so ping-pong writes the
timer about once per period. The same run: 9 to 13 periods, 0.45 to 0.65 µs
a round trip. (Rough, by counting periods; step 8 measures.)

### Step 5 done: full verification run (2026-10-10)

On joist-dev, about 70 minutes:

| Suite | Result |
|---|---|
| `make check` (`model/`) | 149 tests, 0 failed, six builds |
| `make mutate` | 112 of 112 caught |
| `make cbmc` | 7 of 7 operations verified |
| `make tla`, `make tla-mutate` | safe and large configurations hold, the unsafe one leaks as expected; every specification mutation caught |
| `make fuzz`, `make fuzz-sanity` | clean; 7 of 7 seeded bugs found |
| `make check` (`kernel/`) | passing: host tests in five builds, freestanding, `clang-tidy`, `scan-build`, BIOS and UEFI boots, 21 self-tests |
| `make mutate` (`kernel/`) | 36 of 36 caught |
| `fuzz-abi`, `fuzz-elf`, `fuzz-syscall` | clean |
| VM 112 | the scheduler test and the 200 000 round trip ping-pong pass under SeaBIOS and OVMF (`docs/phase3-data`, *Step 5*) |

The model and host suites ran at part 3's tree; parts 4 and 5 changed only
`kernel/arch/x86_64/run.c` and the user test programs, which those suites
don't build, and the kernel's `make check` and the VM 112 boots were run
again on the final tree.

What the run and the real CPU found, all in this step's own work: kernel
time charged to no one (part 4); a test that compared counts from two
differently compiled loops (part 4); a timer write on every exit (part 5).
Nothing in earlier steps' code.

Left for later steps, as recorded above: O(1) run queues (step 9, checked
against the scans); a handshake for starting passive servers on more than
one CPU (step 9); `run.c`'s scheduling glue is tested by boot only, with
three hand-made broken kernels tried once rather than kept as mutations.

Step 5 is done.

### Step 6, part 1: a thread's registers through the caller's memory (2026-10-10)

What the kernel already had from steps 3 and 4: faults from user mode go to
the core's `joist_fault` (a `Call` to the handler), and suspend, resume,
abort and the two set-fault calls are in the ABI. What was missing was the
way for a handler to see and change the faulting thread: calls 70 and 71,
`thread_read_regs` and `thread_write_regs`, with the record in the caller's
memory (`docs/abi.md`, *Thread control and faults*).

- `copy_to_user` (`kernel/sys/usercopy.c`): the kernel's first write to
  user memory. Through the page tables like `copy_from_user`, user half
  only, every page user-writable, and all or nothing: every page is checked
  before a byte is written.
- The two calls in `kernel/sys/abi.c`: the capability's checks, then the
  pointer. Written values need no checking there, because the exit to user
  mode already trusts no thread's record (pc, flags, TLS base).
- `user/lib/joist.h`: the record, `j_thread_read_regs`,
  `j_thread_write_regs`, suspend, resume, abort, kill, the set-fault calls;
  the compile-time agreement test covers the record's layout and the fault
  kinds.

Tests: 5 new host tests (writes across pages; refusals that write nothing;
a property test of 20 000 random writes against a shadow of memory; the two
calls, and their refusals in order). 7 new mutations, all caught. Host
tests: 46.

### Step 6, part 2: faults and thread control seen from user mode (2026-10-10)

`user/faultroot` is a root task that is the fault handler of the tasks it
starts; `user/faulter` is the one program it loads, which faults in the way
the page it shares with the root says. Two new boot self-tests (`fault`,
`ubsan-fault`; 23 in all). What is checked:

| Test | What must hold |
|---|---|
| Resume | an invalid instruction arrives as a message (kind, the task's badge, a capability to the thread); the handler reads the thread's registers, moves the pc past the instruction, replies *resume*; the thread goes on |
| Pager | a write to an unmapped address arrives with the address and the error code (user, write, not present); the handler maps a fresh page there and replies; the same instruction runs again and succeeds, and the value is in the page the handler gave |
| Terminate | a privileged instruction; the handler replies otherwise; the thread is gone (`E_GONE` through its capability) |
| No handler | a division by zero in a task with no handler: the task is destroyed, and the kernel says so in the log |
| Unresolved | a thread's own handler comes before its task's (the badge tells); a fault call that is aborted leaves the endpoint's queue and leaves the thread suspended; whoever holds it can move its pc and resume it |
| Control | suspend stops a spinner, twice is no error, resume starts it, resuming a running thread is `E_STATE`, kill ends it; abort makes a `Recv` that would never end return `E_ABORTED`; registers written before a thread first runs are what it starts with; the TLS base is a field of the record; a kernel address as TLS base and flags with IOPL and the trap flag don't reach the thread; a pc in the kernel half never runs and is a general-protection fault to the handler |

It passed the first time it ran, but for one thing in the test's own
support: the root ran out of the first 128 slots of its capability table
(the second leaf holds its image). `user/lib/loader.h` now hands slots out
through `slot_new`, which grows the table a leaf at a time
(`j_cspace_grow`): the first use of the two-level table from user mode.
The helpers the two test root tasks share moved to `user/lib/testroot.h`.

A host test of part 1 had a fault of its own, found by the UBSan build in
`make check`: an empty write below the test's memory indexed the shadow out
of range. Test corrected; the kernel code was right.

`make check` in `kernel/` passes; VM 112 runs the fault test under SeaBIOS
and OVMF (`docs/phase3-data`, *Step 6*).

### Step 6 done: full verification run (2026-10-10)

One more change after part 2: the kernel's report of a task destroyed for a
fault with no handler now follows the core's verdict (`joist_fault`
returning `E_PEER`) and no longer its own guess from the handler slots, so
a handler slot that is named but holds nothing usable is reported too. The
fault test covers it (an empty handler slot), and the self-test requires
both lines in the log.

On joist-dev, about 70 minutes:

| Suite | Result |
|---|---|
| `make check` (`model/`) | 149 tests, 0 failed, six builds |
| `make mutate` | 112 of 112 caught |
| `make cbmc` | 7 of 7 operations verified |
| `make tla`, `make tla-mutate` | hold; every specification mutation caught |
| `make fuzz`, `make fuzz-sanity` | clean; 7 of 7 seeded bugs found |
| `make check` (`kernel/`) | passing: 46 host tests in five builds, freestanding, `clang-tidy`, `scan-build`, BIOS and UEFI boots, 23 self-tests |
| `make mutate` (`kernel/`) | 43 of 43 caught |
| `fuzz-abi`, `fuzz-elf`, `fuzz-syscall` | clean |
| VM 112 | the fault test passes under SeaBIOS and OVMF (`docs/phase3-data`, *Step 6*) |

The model and host suites ran at part 2's tree; the change above is in
`kernel/arch/x86_64/run.c` and the fault test only, and the kernel's
`make check` and the VM 112 boots were run again on the final tree.

The core needed no change in this step: what step 3 put in the model for
faults and thread control worked in the kernel as modelled. Not done here
and left to the exit step's fault injection: handlers that die while
holding a fault, and faults under load.

Step 6 is done.

### Step 7: preemptible cascades in the kernel (2026-10-13)

**The build VM moved.** pve1 went out of service, and VM 113 with it. A
second build VM with the same pinned toolchain was made on pvehome (VM 114,
`joist-dev2`; `docs/testbed.md`), and `tools/dev/remote.sh` uses it.

**What changed in the kernel.** Until now the kernel ran the core with no
preemption limit: destroying a task swept its whole capability table, and
every task that made unreachable, in one go with interrupts off.

- `run.c` sets the core's limit (`RUN_PREEMPT_SLOTS`, 32 slots): no
  operation sweeps more.
- Every kernel entry sweeps one more piece of what is pending; idle sweeps
  the rest, a piece at a time, looking for wake-ups in between. So a
  destruction always ends and nothing waits for more than a piece.
- `continue` from user mode is bounded by the same limit whatever the
  caller asks (it used to pass the caller's number through, and 0 meant
  all of it): one host test, two mutations, both caught.

**A rule tried first, and why it went.** The first version went on sweeping
for as long as the thread that entered the kernel was still the one to run
and no deadline or budget end was due, so that the caller of a destruction
got it done at once. The test below found the hole in about one round in
ten: when a timer interrupt landed while the *ticker* was running, the
ticker was "the thread that entered", and the rest of the cascade was swept
in its name. One piece per entry, and `continue` for whoever wants more,
has no such case.

**The test** (`user/cascaderoot`; three boot self-tests, 26 in all). The
root builds a chain of 48 tasks, each holding 100 endpoints of its own and
the only capability to the next, and destroys the first: 4 848 capabilities
swept and as many objects destroyed. A ticker of higher priority sleeps a
tenth of a cascade's length again and again and records its longest wait.
Required: the ticker never waits as long as half a cascade, it wakes at
least three times per cascade, and after 20 rounds the root can create as
many tasks as before (62). It runs on a kernel built with larger tables
than the default (8 192 objects, 64 tasks), because with the default ones a
whole cascade is under half a millisecond and too short to measure
against: preemption matters as the tables grow.

`cascade-nopreempt` boots the same test on a kernel built never to preempt
(`RUN_PREEMPT_SLOTS=0`) and requires it to **fail**: a standing check that
the test can tell, which the scheduler and fault tests only had by hand.

| | QEMU (TCG) on joist-dev2 | VM 112 (KVM) |
|---|---|---|
| A cascade alone | about 3.8 ms | 0.5 to 1.2 ms |
| Ticker's period | about 380 us | 54 to 113 us |
| Its longest wait during 20 cascades | 470 to 750 us | 71 to 149 us |
| The same with nothing else running | 400 to 510 us | 71 to 123 us |
| Kernel that never preempts: longest wait | 3.6 to 4.0 ms (fails, as required) | 1.3 ms (fails) |

**What the real CPU found this time** was in the test, and in the test
bed: VM 112 now shares its host with the build VM, and one run in five had
a round where the virtual CPU lost its host CPU for about a millisecond.
Such a round shows twice (the ticker waited and the cascade took that much
longer), which a kernel hold-up doesn't, so the test sets aside rounds that
took over one and a half times the median (four at most) and requires all
the others to be good. The first rule's fault still fails it: there the
cascade is no longer than usual.

Two slips of my own on the way, both in the test's support: a host test
that read the sweep cursor as a count of slots (past the first leaf it
moves a leaf at a time), and a self-test directory reused across builds
with different table sizes (stale objects; the kernel crashed on boot).

`make check` in `kernel/` passes: 47 host tests, 26 self-tests.

### Step 7 done: full verification run (2026-10-13)

On joist-dev2 (VM 114), the first full run there, at the step's commit;
about 25 minutes, against about 70 on VM 113.

| Suite | Result |
|---|---|
| `make check` (`model/`) | 149 tests, 0 failed, six builds |
| `make mutate` | 112 of 112 caught |
| `make cbmc` | **did not run** (see *Correction*, below, 2026-10-13); the core was unchanged since step 6, where it passed |
| `make tla`, `make tla-mutate` | hold; every specification mutation caught |
| `make fuzz`, `make fuzz-sanity` | clean; 7 of 7 seeded bugs found |
| `make check` (`kernel/`) | passing: 47 host tests in five builds, freestanding, `clang-tidy`, `scan-build`, BIOS and UEFI boots, 26 self-tests |
| `make mutate` (`kernel/`) | 45 of 45 caught |
| `fuzz-abi`, `fuzz-elf`, `fuzz-syscall` | clean |
| VM 112 | the destruction test passes under SeaBIOS and OVMF, and fails on the kernel that never preempts (`docs/phase3-data`, *Step 7*) |

That every suite gives the results it gave on VM 113 is also the check of
the new build VM. The core needed no change in this step.

Step 7 is done.

### Step 8: the fast path and `ipcbench` (2026-10-13)

**The benchmark first.** `user/ipcbench` is both ends of a round trip;
`user/benchroot` runs it in two tasks, in one task, and with a server that
has a scheduling context of its own, and a system call that does nothing
for comparison. The first figures, before any change, already said where
the time went: a round trip in one address space cost twice a do-nothing
system call and no more. The cost was the way into and out of the kernel,
not the IPC.

**The fast path, as the plan has it: the direct switch.** The core's IPC
operations stay the only implementation of what a `Call` and a `ReplyRecv`
do. What is fast is the decision after them: when the caller's scheduling
context has gone to the other thread (to a waiting passive server; back to
the caller with the reply, no next caller queued), no timeout was set and
nothing is due, the kernel runs the other thread at once, with no scan for
who should run and none for the next timer event.

- `joist_sched_handed` (core) says when that is certain to be the
  scheduler's own choice; it is never wrong, only sometimes unsure.
- **A change of rule that this needed, model first:** a context's place in
  line among its equals goes with it. A server starting on a caller's
  context takes the caller's place, and the reply gives it back. Before, a
  woken server went behind its caller's equals: a call to a passive server
  gave up the turn, which nobody had intended. (`docs/abi.md`,
  *Scheduling*.) All 149 earlier model tests pass unchanged.
- **The differential test** (`model/test/test_handover.c`): random worlds
  of clients at a few priorities with budgets that run out, passive
  servers that also call each other, some timeouts; 80 037 operations;
  whenever `joist_sched_handed` names a thread (20 112 calls, 16 134
  replies), `joist_sched_pick`, the specification, must choose the same
  one, and the next timer event must be no earlier than before. It found a
  fault in my first version of the rule: the place was copied, so two
  threads could hold the same one and the tie went the other way. Places
  now move. Seven mutations, all caught; three more in the system-call
  layer (`abi_env.handover`), caught by a host test.

**The way in and out**, each measured before the next:

| Change | Round trip in one address space, cycles |
|---|---|
| As step 7 left it (converted from counts; approximate) | about 1 700 |
| The direct switch | about 1 590 |
| The clock read as one multiplication, not two divisions; no `lfence`; the TLS base written only when it changes | about 1 100 |
| `sysretq` for a thread that came in by `syscall` | 764 |

and for two address spaces PCID (identifier = the task's number plus one;
a space whose pages were unmapped while it wasn't current, or whose number
was reused, is flushed at its next switch): about 1 800 to 1 700, then
1 356 with `sysretq`.

- `sysretq` needs what the ABI already says (the `syscall` instruction
  clobbers `RCX` and `R11`): the kernel keeps, per thread, whether every
  register counts (new, interrupted, faulted, or written through a
  capability: then `iretq`). NMI and machine check now arrive on a stack
  of their own, as the double fault did, since the `sysretq` exit runs
  one instruction on the user's stack.
- PCID exists only on a real CPU (QEMU's emulated ones have none), so
  three self-tests run under KVM inside the build VM (`pcid-fault`,
  `pcid-pingpong`, `pcid-cascade`), and say SKIPPED where there is no
  `/dev/kvm`. The fault test gained the case PCID could break: a page
  unmapped from a task that isn't running must fault at its next touch. A
  kernel with the stale mark removed, tried once by hand, fails it.

**Results** (VM 112, CPU cycles per round trip, medians; ten runs each;
`docs/phase3-data`, *Step 8*):

| | Slow path only | Fast path |
|---|---|---|
| A system call that does nothing | 580 | 372 |
| Two address spaces, passive server | 1 896 | 1 356 |
| One address space, passive server | 1 292 | 764 |
| Two address spaces, server with its own context | about 10 000 | about 9 750 |

Stable by the plan's measure: over ten runs the medians move 1.1 % at
most and the 99th percentiles under 12 %.

**Making the numbers mean something** took as long as the fast path. On
this host the same kernel gave medians 30 % apart from run to run. In the
order found: the CPU's clock moves between turbo steps while the time
stamp counter doesn't (so the benchmark carries a ruler, a chain of
multiplications of known length in CPU cycles, and reports in those); the
first tenths of a second after the VM starts are slow in some runs (it
waits a second); short slow stretches on a shared host (it reports the
median of twenty segments). `docs/testbed.md` has the list. The build VM
was moved off the test VM's cores.

**The regression gate** (`kernel/test/ipcbench.baseline`, checked by
`bench-gate` in `make selftest`): on a real CPU, no median more than 25 %
above its baseline. The slow-path kernel's figures are 40 to 70 % above,
so it would fail it.

**Not done, and said so:** the case of a server with its own context is
ten times slower than the others under KVM, because every change of
context sets the timer and that leaves the virtual machine; a
TSC-deadline timer, or bare metal, would tell how much of it is real. The
kernel still copies every register twice on the way through. Neither is
needed for the exit criterion; both are recorded for Phase 13.

`make check` in `kernel/` passes: 49 host tests, 31 self-tests and the
gate. `make test` in `model/`: 152.

### Step 8 done: full verification run (2026-10-13)

On joist-dev2, at the step's commit, about 25 minutes:

| Suite | Result |
|---|---|
| `make check` (`model/`) | 152 tests, 0 failed, six builds |
| `make mutate` | 119 of 119 caught |
| `make cbmc` | **did not run** in this run (see *Correction*, below); run afterwards on this commit's tree: 7 of 7 operations verified |
| `make tla`, `make tla-mutate` | hold; every specification mutation caught |
| `make fuzz`, `make fuzz-sanity` | clean; 7 of 7 seeded bugs found |
| `make check` (`kernel/`) | passing: 49 host tests in five builds, freestanding, `clang-tidy`, `scan-build`, BIOS and UEFI boots, 31 self-tests, the benchmark gate (376, 1 356 and 764 cycles against baselines of 376, 1 356 and 764) |
| `make mutate` (`kernel/`) | 50 of 50 caught |
| `fuzz-abi`, `fuzz-elf`, `fuzz-syscall` | clean |
| VM 112 | the benchmark, ten runs each of the fast and the slow-path kernel (`docs/phase3-data`, *Step 8*) |

Step 8 is done.

### Correction: CBMC did not run in the full runs of steps 7 and 8 (2026-10-13)

The records of steps 7 and 8 above said "`make cbmc`: 7 of 7 operations
verified" for their end-of-step runs. That was wrong when written. On the
new build VM (joist-dev2, from step 7) CBMC could not start: it uses `gcc`
as its preprocessor, the VM had none, and `make cbmc` failed in a second
with a parsing error. I reported it as passing because the loop I ran the
suites with printed the exit status of a pipe through `tail`, which is
`tail`'s, always 0; and I looked for failure words, not for the line that
says CBMC succeeded. The commit messages of `de76d0f` and `92d20da` carry
the same claim and are left as they are (pushed); this section corrects
them.

What was and wasn't affected:

- **CBMC** is the only suite that didn't run. Both saved logs were checked
  again for any failing make target: only that one.
- **Step 7** changed nothing in the core, which CBMC had verified at step
  6 on the old VM.
- **Step 8** changed the core (a context's place in line). CBMC was run
  afterwards on exactly that commit's tree (`git archive 92d20da`): 7 of 7
  operations verified. So the statement is true of the code, and was not
  established when made.
- Step 9's part 1 changes the thread record again; CBMC run on it: 7 of 7.

What changed so it can't recur: `gcc` is installed on joist-dev2 and listed
in `docs/testbed.md`; the suites are now run by `tools/dev/fullrun.sh`,
which records make's own status per suite, counts caught and surviving
mutations from the whole log, prints the lines that say CBMC and TLA+
succeeded, and ends with one line that names any suite that failed.

