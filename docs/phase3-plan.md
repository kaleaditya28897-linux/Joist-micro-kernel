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

