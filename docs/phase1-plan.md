# Phase 1 plan: the semantic model on the host

Plan §11, Phase 1: *build a host-side C model of the kernel's object model,
run it under the host test harness with all sanitizers, test invariants;
optional TLA+.* **Exit: invariants proven or property-tested; the API written
down as `docs/abi.md`.**

This file turns that into concrete work. It is a plan, not a decision: where
it chooses something the decisions don't, the choice is marked and can be
changed without a journal entry until Phase 3 builds on it.

## Scope

The model implements the object model of the accepted decisions, as a C17
library with no kernel or hardware in it:

| Object (§5.2) | Behaviour the model must have | Decision |
|---|---|---|
| Task | address space *handle* (no pages) + capability space + threads; created from donated memory | 0004, 0011 |
| Thread | belongs to one task; blocked/runnable state; a fault-handler and a thread-control capability | 0004, 0009, 0010 |
| Capability | rights bits (send, receive, send-once, grant), badge, derivation that never gains rights | 0001 |
| Endpoint | synchronous rendezvous: `Send`, `Recv`, `Call`, `Reply`, `ReplyRecv`, timeouts | 0001 |
| Reply capability | minted by `Call`, consumed by `Reply`, invalidated if the server side dies | 0001 |
| Notification object | fixed-width bit-set; `Signal` sets, `Wait` clears and returns; IRQs, timers, lifetime events | 0001, 0005, 0014 |
| Lifetime events | no-senders per badge (preallocated accounting); dead-name on endpoint destruction | 0001, §5.4 |
| Memory accounting | every object paid for from donated memory; no allocation on the client-arrival or client-death path | 0003, 0004 |
| Root task | starts with every capability, delegates, drops what it doesn't keep | 0013 |

**Out of scope:** pages, page tables and faults (0007/0008, Phase 4);
scheduling *policy* (only the run/blocked state and donation are modelled);
real SMP. Concurrency is modelled where invariants depend on it (deletion vs.
in-flight IPC); otherwise the model is single-threaded and deterministic, so
failures replay from a seed.

**Constraint carried from §8.1:** the model's core is written so it can move
into the kernel. That means no `malloc` in the core (objects come from
explicitly donated pools), no recursion, no VLAs, fixed-width types, and
every operation's work bounded. The test harness may use the host freely.

## Invariants

Each is to be **property-tested**: random operation sequences from a seed,
checked after every step against these statements (§11's exit criterion).
The numbering is for cross-reference from tests and `docs/abi.md`.

| # | Invariant | From |
|---|---|---|
| I1 | **No leaked capabilities.** Destroying a task frees every capability slot it held, and every object's reference count equals the number of capabilities naming it. | §11 |
| I2 | **Notifications are never lost.** A `Signal` before, during or after a `Wait` is observed by exactly one later `Wait`; bits only clear when returned. | §11, 0001, 0014 |
| I3 | **Bounded memory.** After "boot" the model allocates only from donated pools; total use never exceeds total donation; a failed allocation is an error, never a block. | §11, 0003 |
| I4 | **No-senders fires exactly once**, when (and only when) the last send capability carrying a badge is deleted. | §5.4, 0001 |
| I5 | **Dead names are prompt.** After an endpoint is destroyed, every use of a capability to it fails, and each registered holder is notified at most once. | §5.4, 0001 |
| I6 | **Reply capabilities are single-use.** `Reply` consumes it; a second use fails; if the server side dies, the blocked `Call` returns an error instead of hanging. | 0001 |
| I7 | **Badges can't be forged.** A receiver sees exactly the badge minted into the sender's capability; only a holder of grant authority mints badges. | 0001, 0005 |
| I8 | **Rights never grow.** Every derived capability's rights are a subset of its source's. | 0001 |
| I9 | **Every operation is bounded.** Each operation's step count stays under a fixed bound independent of user-controlled sizes (the host stand-in for WCET). Revoke may *block the caller*; it never allocates. | 0003, §5.1 |
| I10 | **Blocking always ends.** Every blocking operation returns, on rendezvous, timeout or peer death; no reachable state blocks forever without a timeout armed. | §5.3 |

## The parameters this phase settles

0001 left five parameters open "before Phase 3 coding". The model is where
each gets its number, recorded as an amendment to 0001:

| 0001 item | How the model settles it |
|---|---|
| 1. Message registers: number and width | Size by what the hot MIG messages need in registers. The Phase 0 RPC counts (`io_read`, `dir_lookup`, `io_stat`) give the target messages. |
| 2. Badge width; kernel- or server-allocated | Model both allocation schemes against I7 and the no-senders accounting of I4. |
| 3. Notification bit-set width | **Load-bearing:** 0005's `select` protocol and 0014's futexes both build on it. Model `select` over many descriptors and futexes over many locks, and measure what happens when events outnumber bits. |
| 4. Capability-space representation | Flat array vs. guarded page table against I1, I9 and 0011's batched capability copy for `fork`. |
| 5. Is a bare `Send` needed? | Implement everything Phase 0 found with `Call`/`Reply` only, and see whether anything needs `Send`. |

## Method

- **Unit tests** per object and operation (`model/test/`, the in-tree harness).
- **Property tests:** a seeded generator of operation sequences, with I1–I10
  checked after every step. A failure prints its seed and step, and replays
  exactly.
- **libFuzzer** drives the same sequence executor from fuzz input (the
  runtime is installed).
- **Every test under every sanitizer:** `make check` runs plain, ASan, UBSan,
  MSan and TSan. `make sanity` proves each sanitizer is live by catching a
  seeded bug.
- **Optional, per §8.1 and §11:** a TLA+ model of IPC plus capability
  deletion, for the races the deterministic model can't explore; CBMC on the
  capability-space code.

## Order of work

1. Capability space and derivation (I1, I8), on donated pools (I3).
2. Endpoints, `Call`/`Reply`/`ReplyRecv`, timeouts (I6, I10).
3. Notifications (I2), then no-senders and dead-name on top of them (I4, I5).
4. Tasks, threads, the root task and delegation (0004, 0013), and task
   destruction (I1 end to end).
5. The property-test generator and libFuzzer harness over everything above.
6. Settle 0001's five parameters (amendment to 0001), using items 1–5.
7. Write `docs/abi.md` from the model's API, not before it.
8. Optional: TLA+ for deletion vs. in-flight IPC.

## Tooling

In place on the workstation (Void Linux, clang 21.1.8), verified 2026-10-01:
clang with all four sanitizer runtimes and libFuzzer, `clang-format` (the
style is `/.clang-format`), Java 11 (enough for the TLA+ tools).

**Installed 2026-10-04**, per user, under `~/.local` (no root needed):

| Tool | Version | Source, SHA-256 of the download | For |
|---|---|---|---|
| TLA+ tools (`tla2tools.jar`, run as `tlc`) | v1.7.4 (TLC2 2.19) | github.com/tlaplus/tlaplus release v1.7.4, `936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88` | work item 8 |
| CBMC | 6.11.0 | github.com/diffblue/cbmc release `ubuntu-24.04-cbmc-6.11.0-Linux.deb`, `b3721aa541038384d7801ea3aeabbcddc3e8845ac8f1cbff637cf8dec7481ac8`, extracted to `~/.local/opt/cbmc-6.11.0` | §8.1 bounded model checking |

`~/.local/bin/tlc` is a two-line wrapper: `java -cp tla2tools.jar tlc2.TLC`.

**Still missing; they need root** (`sudo xbps-install -S clang-tools-extra
clang-analyzer lld llvm`, the meta packages matching clang 21):

| Tool | For |
|---|---|
| `clang-tidy`, `scan-build` | §8.1 static analysis on every commit |
| `ld.lld`, `llvm-ar`, `llvm-objdump` | §13 toolchain; not needed by the host model, needed from Phase 2 |

## Progress

### Work item 1: capability space and derivation (2026-10-02)

`model/include/joist/model.h`, `model/src/cspace.c`. Capability spaces
(a flat array for now; 0001 item 4 is still open), objects reference-counted by
the capabilities naming them, per-space object quotas charged to the
creator and refunded on destruction, `derive` (rights masked), `mint`
(requires GRANT on an unbadged source; badges immutable), `delete`, and
`cspace_destroy`. All state is in one caller-provided struct; no `malloc`.

| Check | Result |
|---|---|
| Unit tests (`test/test_cspace.c`) | 10 tests, every operation and error path |
| Property test (`test/test_props_cspace.c`) | 200 seeds × 2 000 steps: model and an independent shadow agree on every return code and the whole state after every step; `joist_check_invariants` clean throughout. Covers I1, I3, I7, I8 |
| Coverage (printed by the run) | every operation reaches success and each of its error codes; the generator is biased to occupied slots so that deep states (many capabilities to one object) are built: ~50 000 successful derives per run |
| `make mutate` (`test/mutations.txt`) | 6 of 6 injected bugs caught, each within the first seed: rights growing (I8), a missing reference count (I1), a missing quota refund (I3), re-badging (I7), a destroyed space leaking objects (I1), quota ignored (I3) |
| `make check` | all tests under plain, ASan, UBSan, MSan and TSan |

Not yet: **revoke** (needs a derivation tree; 0003 lets it block the caller,
so it gets its own step), and transfer of capabilities between spaces
(comes with IPC, work item 2).

### Work item 2: endpoints and synchronous IPC (2026-10-02)

`model/src/ipc.c`. Threads (a minimal table owned by a capability space, a
fixture until work item 4), endpoint queues (one FIFO, senders or receivers,
never both; doubly linked through thread records for O(1) removal),
`Send`/`Call`/`Recv`/`Reply`/`ReplyRecv`, model time with `joist_tick`, and
peer death. A blocking operation either completes at once or returns
`JOIST_BLOCKED`; the woken thread's record holds the outcome.

| Check | Result |
|---|---|
| Unit tests (`test/test_ipc.c`) | 15 tests: both rendezvous orders, `Call`/`Reply` both orders, single-use reply (I6), badge delivery (I7), rights/type/range errors, timeout 0, expiry exactly at the deadline, a caller's timeout leaving the reply stale, endpoint destruction and server death waking waiters (I10), FIFO order, stale replies not reaching a reused thread slot, space destruction taking its threads |
| Property test (`test/test_props_ipc.c`) | 200 seeds × 2 000 steps of random IPC, ticks, thread churn and endpoint deletion against an independent shadow of the queue and reply rules: return codes, every thread field and every queue's order agree after every step. At the end of each seed all deadlines are run out (nothing with a deadline may still be blocked, I10), then every space is destroyed (no thread or object may survive, I1) |
| Coverage | every return path of every operation; wake-ups by reason: rendezvous ~11 000, timeout ~23 000, endpoint destroyed (`E_GONE`) ~2 000, server death (`E_PEER`) ~370 |
| `make mutate` | 13 of 13 caught: the 6 capability mutations plus a reusable reply capability (I6), server death not waking the caller (I10), endpoint destruction not waking waiters (I10), a dropped badge (I7), timeouts one tick late, a reused thread slot resetting its call sequence, and `Recv` allowed while holding a reply. `make mutate` now runs the whole suite, so a unit test catching a mutation counts |
| `make check` | 30 tests under plain, ASan, UBSan, MSan and TSan |

**Semantics the model had to choose** (0001 doesn't fix them; `docs/abi.md`
will inherit them unless Phase 1 revisits them):

1. **The reply capability is per thread** (one per server thread), not a
   capability-space slot. A server thread therefore holds at most one
   outstanding reply. Hurd servers answer each request on the thread that
   received it, so that suffices for now; deferred replies would need reply
   capabilities in slots.
2. **`Recv` while holding an unused reply capability is refused**
   (`JOIST_E_STATE`). Overwriting it would strand the caller; `ReplyRecv`
   replies first.
3. **A `Call`'s timeout covers the whole call**, send and reply wait. A
   `Call` with timeout 0 is refused (`JOIST_E_TIMEOUT`), since a call always
   waits.
4. **A reply to a caller that stopped waiting** (timed out, or destroyed)
   returns `JOIST_E_PEER` and still consumes the reply capability.
5. **A reply capability names (thread slot, call sequence),** and the
   sequence survives the slot's reuse, so a stale reply can never reach a new
   thread in the same slot.

Not yet: capabilities passed in messages (grant), and scheduling-context
donation (only the run/blocked state is modelled, as planned).

### Work item 3: notifications, no-senders, dead names (2026-10-02)

`model/src/notify.c`. Notification objects (a 64-bit word plus a waiter
queue), `Signal` and `Wait`, endpoint death, per-badge send accounting with
no-senders firing (I4), and dead-name registrations (I5). Every capability
creation and deletion now goes through two hooks (`joist__cap_added`,
`joist__cap_removed`), so the receive counts and badge accounts can't drift
from the capability tables. `joist_check_invariants` verifies both against
a full recount.

**A correction to work item 2's model.** An endpoint used to die with its last
capability of any kind. Then nobody could ever hold a capability to a dead
endpoint, and Mach's dead names couldn't exist. An endpoint now **dies when
its last capability with RECV is deleted** (the server is gone). Remaining
send capabilities then fail with `JOIST_E_GONE`, and the memory goes with the
last capability. Since derive can't add RECV (I8), death is permanent. The IPC
shadow was updated to the new rule.

| Check | Result |
|---|---|
| Unit tests (`test/test_notify.c`) | 14 tests: a signal before a wait is kept and a signal is never delivered twice (I2), waiters, rights/badge/type, timeouts, a freed notification waking its waiters, no-senders firing exactly once on the last sender, badges independent, non-senders not counted (I4), a full accounting table failing only `mint`, endpoint death with send capabilities remaining, dead names firing once (I5), registration errors, bindings dropped with their notification, no-senders silent after death |
| Property test (`test/test_props_notify.c`) | 200 seeds × 2 000 steps against a shadow that predicts every word, waiter queue, endpoint death, no-senders firing and dead-name firing, in the model's order of effects within a delete; teardown must leave no object, account or registration. Per run: ~310 no-senders firings, ~330 dead-name firings, ~9 900 endpoint deaths, ~9 700 notifications freed |
| Small-capacity build (`make small`, new in `make check`) | the same tests with 32 objects, 8 threads, 4 badge accounts and 4 dead-name entries, under ASan + UBSan, so exhaustion paths are hit constantly (accounting-full mints, object and thread table exhaustion). Dead-name table exhaustion is covered by a unit test only; the property runs don't fill it |
| `make mutate` | 22 of 22 caught, including 9 new: bits lost or delivered twice (I2); no-senders never firing, firing early, or firing after death (I4); an endpoint that never dies, a dead name firing repeatedly, a dead endpoint still usable (I5); a binding left dangling. Two IPC mutations whose target text had been refactored away were re-targeted, which is what the "text not found" check is for |
| `make check` | 45 tests under plain, ASan, UBSan, MSan, TSan and small |

**Semantics the model chose** (to revisit in Phase 1, or carry into `docs/abi.md`):

1. **Endpoint death is receive-side** (above).
2. **`Signal` ORs the capability's badge** into the word, seL4 style; an
   unbadged signal is refused. Who signalled is therefore unforgeable (I7).
3. **No-senders signals the client's badge value.** For a server to tell its
   clients apart this way, badges must be distinct bits or bit groups of the
   notification word. That ties 0001's item 2 (badge width) to item 3
   (notification width), and is the strongest argument yet for settling them
   together.
4. **No-senders tracks badged send capabilities only.** Unbadged senders aren't
   distinguishable, so there is nothing to report about them.
5. **Accounts are allocated only by `mint`**; derive and delete never allocate.
   A full table fails the mint and changes nothing.
6. **A dead-name request on an already dead endpoint returns `JOIST_E_GONE`**
   rather than firing at once.
7. **`Wait` is refused while holding a reply capability**, as `Recv` is.

### Work item 4: tasks, threads, the root task, task destruction (2026-10-02)

`model/src/task.c`. Tasks and threads are now objects named by capabilities
with a CONTROL right (`JOIST_R_CONTROL`, the same bit as SEND). Deleting the
last capability to a live task destroys it, and deleting the last capability
to a live thread kills it, so a task nobody can name can't leak (I1). Boot
creates the root space holding the whole memory budget; it has no task object
and can't be destroyed (0013). `joist_task_create` donates quota from the
creator, which pays the donation plus one for the task object (0004), and
records the creator as the new space's parent. `joist_thread_spawn` is paid
for by the spawner, not by the task the thread runs in.

**Destruction, end to end.** Destroying a task marks its task object dead
(remaining capabilities fail with `JOIST_E_GONE`), kills its threads (callers
blocked on them wake with `JOIST_E_PEER`, as before), and deletes its
capabilities. If that deletes the last capability to another live task, that
task goes too. The cascade runs from a work list (`k->pending`), never by
recursion, and each space is destroyed once, so cycles (a task holding its own
capability, two tasks holding each other's) terminate. Whatever the dead task
still owns goes to its parent: its unspent quota, the charges for objects it
paid for that are still alive (because another task holds a capability), and
its children, which are re-parented (I3). Fixture spaces made by
`joist_cspace_create` have no parent; their quota enters the budget at
creation and leaves it at destruction, so conservation can be checked in
every test.

**I3 is now checked as conservation**: the sum of every live space's quota and
every space's charges equals the budget, after every step. The invariant
check also verifies that task and thread objects and their spaces and threads
name each other, that parent chains are live and acyclic, and that no
destruction is left pending between operations.

| Check | Result |
|---|---|
| Unit tests (`test/test_task.c`) | 19 tests: boot and the root's protection (0013), donation and its errors, the space table filling up, tasks and threads refused by `obj_create`, quota and charges returned on destruction, shared objects re-charged to the parent, the last task capability destroying the task, a three-deep cascade, reachable children re-parented, a self-referencing task, grant and rights and type checks, thread spawn, kill, errors and exhaustion, the last thread capability killing the thread, task destruction waking a blocked caller (I10), and fixture quota leaving the budget |
| Property test (`test/test_props_task.c`) | 200 seeds × 2 000 steps of task, thread, grant, derive and delete operations against a shadow that destroys tasks by **recursion at the moment they become unreachable**, where the model uses a work list drained afterwards. The final states must agree, which checks that a cascade's order doesn't change where quota and charges end up. Every result code of every operation is reached; about 1 570 operations per run destroy more than one task |
| `make mutate` | 36 of 36 caught, including 14 new: children not re-parented, quota or charges not handed to the parent, a task object not marked dead, no cascade, a thread capability's deletion not killing the thread, the donation off by one, CONTROL or GRANT not checked, a dead task usable, the work list not drained, the root destroyable, a fixture's budget not reduced, a killed thread's object not marked dead |
| `make check` | 65 tests under plain, ASan, UBSan, MSan, TSan and small |

**Semantics the model chose** (to revisit in Phase 1, or carry into `docs/abi.md`):

1. **A task dies with its last capability**, like an endpoint with its last
   RECV capability. There is no separate "terminate" right; CONTROL is it.
2. **Orphans go to the grandparent**, with their charges. The alternative,
   destroying a task's children with it, would make a server that outlives its
   launcher impossible (an init-like reaper has to be able to inherit).
3. **The spawner pays for a thread**, so a task with no quota can still be
   given threads by its creator.
4. **Capability transfer is still open.** The tests copy capabilities between
   spaces with an internal `joist__cap_grant` (needs GRANT; rights masked) as
   a stand-in until transfer in messages is modelled.

**Open, recorded:** a cascade is bounded by the table sizes but not per
operation (I9). This is the case 0003 means by "revoke may block the caller":
the kernel's version has to be preemptible, with the work list as its
continuation. Settling that is part of item 6 or Phase 2.

### Work item 5: whole-model property test and libFuzzer harness (2026-10-03)

`model/test/exec.[ch]` is one **sequence executor** over every operation of
items 1–4 (23 operations, interleaved). It decodes each operation and its
arguments from a stream of choices, applies it, and checks after every step:

- every structural invariant (`joist_check_invariants`: I1, I3, I4, I5, I6,
  I10's queue structure, 0013);
- **I8** directly: a derived, minted or granted capability has no right its
  source lacks;
- **I2** directly: after a signal, every bit that was pending and every
  signalled bit is still pending or was received by a waiter;
- **I6** directly: a successful reply consumes the reply capability and
  reaches the caller with that step's message;
- **I9** as a work bound (below).

At the end of each sequence a **teardown** destroys the fixtures and deletes
the root's capabilities, so every task should go by cascade alone, then checks
that nothing is left: no object, thread, no-senders account or dead-name
registration, and the whole budget back in the root (I1, I3), with no thread
blocked except on a reply its live server still owes (I10).

Two drivers feed it: `test/test_props_all.c` (a seeded RNG, in `make check`)
and `fuzz/fuzz_model.c` (libFuzzer: the fuzz input is the choice stream, one
byte per choice of up to 256 values; `make fuzz`). A fuzzer crash replays in
the fuzz binary, and a property failure replays from its seed. Every sequence
starts from a small world in the root (an endpoint and a send-only copy, a
notification and two copies with different badges, three threads), so both
drivers reach IPC at once.

**I9, measured.** Every loop an operation runs now counts a unit of work
(`JOIST_WORK`, the host stand-in for WCET; invariant checks don't count). Two
closed-form bounds come from the table sizes alone (`test/exec.h`). An
operation that frees nothing must stay under four sweeps of every table
(1 856 units at the default sizes). One that can free (delete, destroy, kill)
must stay under the cascade bound: every space destroyed at most once, every
object freed at most once, every capability deleted at most once, each at a
fixed cost (186 368 units). Worst cases seen over 400 000 steps:

| Operation | Worst work | Bound |
|---|---:|---:|
| `obj_create`, `mint`, `task_create`, `spawn` | 76–94 | 1 856 |
| `derive`, `grant`, `tick`, `th_create`, `deadname` | 10–32 | 1 856 |
| IPC and notification operations, `kill`, `th_destroy` | 0 | 1 856 / 186 368 |
| `delete` | 2 082 | 186 368 |
| `space_destroy` | 2 340 | 186 368 |
| `task_destroy` | 2 980 | 186 368 |

Thread operations and IPC are constant-time in this model (queues are linked
lists). Linear scans for a free table slot are the largest non-cascading cost;
a kernel would use free lists. Cascades are about 30 times dearer than the
worst non-cascading operation, which is the measured size of the open
preemptible-revoke question.

**Making the fuzzer find bugs.** `make fuzz-sanity` injects bugs from
`test/mutations.txt` and requires the fuzzer to crash on each, the fuzzing
analogue of `make sanity`. The first version caught 4 of 7, and the misses
were instructive:

- **Reply capability reusable (I6), lost signal bits (I2):** state stays
  consistent, so invariant checking can't see either. That led to the direct
  I2 and I6 checks above. The first I2 check was also wrong: it tested only
  the new bits, and the mutant loses the *old* ones.
- **No cascade on the last task capability (I1):** an unreachable task stayed
  alive, and nothing told a task space from a test fixture. Spaces now carry a
  `fixture` flag, and a new invariant says a live space with no task object
  must be the root or a fixture.
- **Fuzzer reach:** with those checks in, the fuzzer still missed I2 and I6.
  A coverage dump (`JOIST_FUZZ_STATS=1`) showed it almost never had a running
  thread: nearly every IPC call failed with `E_STATE`. Three fixes: the
  starting world, denser input encoding, and `-len_control=0` so inputs can be
  long at once.
- **Biased picks:** even then, I2 was caught in one fuzz-sanity run and
  missed in the next. The executor picked "the first matching slot after a
  random start", which favours the first of every run of adjacent matches.
  Here that was the unbadged notification, so most signals failed with
  `E_BADGE`. All four pickers now choose uniformly among the matches.
- **Still flaky, so seeds:** the next run caught I2 and missed I6. A
  15-minute coverage dump showed why: libFuzzer favours small byte values,
  which decode to the first operations (`space_create`, `task_create`), and
  successful replies were about 50 times rarer than under the seeded RNG.
  `fuzz/make_seeds.c` now records the property test's first 32 sequences in
  the fuzz input encoding. Each file replays its sequence exactly (checked:
  32 × 1 000 steps, replies at the seeded rate), and `make fuzz` starts from
  them.

`make fuzz-sanity` therefore runs each bug twice. **From the seed corpus**
(pass/fail), the fuzz binary must crash. That guarantees it reaches at least
what the property test reaches, though the seeds often hit the bug on replay
alone. **From an empty corpus** (reported, not failing) shows whether
coverage-guided search finds the bug unaided.

**Results** (raw logs and their SHA-256 sums in `docs/phase1-data/item5/`):

- **`make fuzz-sanity`:** all 7 bugs (one or more per area: I1, I2, I3, I5,
  I6, two task bugs) crash the fuzzer from the seed corpus within a second.
  From an empty corpus, coverage-guided search alone finds each one too: five
  at once, the lost signal bits in 1 s, the reusable reply capability in 15 s.
- **`make fuzz`, 15 minutes from an empty corpus plus the seeds:** no crash.
  8 080 inputs, 6.6 million steps (about 7 400 a second, under ASan and
  UBSan with every invariant checked after every step), 546 new corpus
  entries. Successful replies went from 353 in an earlier 15-minute run
  without seeds to 12 924, which shows what the seeds change.

**Found: task cycles aren't collected (open, for a decision).** Two tasks
holding each other's task capabilities outlive their last outside capability,
because reference counting can't collect a cycle (`task_cycle_outlives_outside_caps`
pins the current behaviour). Quota stays conserved but is stranded until
someone destroys one of them directly. The whole-model test reaches this
in 18 of 200 sequences. Mach and seL4 have the same property. The options:

1. **Accept it, as seL4 does:** whoever creates tasks (in the Hurd, `proc`)
   keeps a CONTROL capability to each, so it never loses the ability to
   destroy them. The cost is a protocol rule, enforced nowhere.
2. **Parent authority:** let a task destroy any descendant through the
   parent chain the model already records (0004's donor relation), without a
   capability to it. That makes the parent chain an authority path, which
   capability purists avoid, but quota already flows along it.
3. **A collector:** detect unreachable cycles (in the kernel, or a
   privileged user task given a "list my descendants" call). The most
   machinery, and the kernel version breaks I9.

The model takes no position yet. Option 1 needs nothing new, and option 2 is
a small, bounded addition to `task_destroy`.

| Check | Result |
|---|---|
| Whole-model property test (`test/test_props_all.c`) | 200 seeds × 2 000 steps, 23 operations, every invariant after every step, clean teardowns. Wake-ups over the run: 4 668 by rendezvous, 13 494 by timeout, 659 by endpoint death (`E_GONE`), 30 by server death (`E_PEER`) |
| Unit test | `task_cycle_outlives_outside_caps` (`test_task.c` now has 20) |
| `make mutate` | 36 of 36 caught. No new mutations: item 5 adds checks, and `make fuzz-sanity` reuses the existing ones |
| `make check` | 67 tests under plain, ASan, UBSan, MSan, TSan and small |

### Work item 6: settling 0001's five parameters (2026-10-03)

The result is an **amendment to decision 0001**, at the end of
`docs/decisions/0001-native-ipc-model.md`, **accepted 2026-10-03** as
written; on acceptance the model's bare `Send` was removed. In short:

| 0001 item | Proposed | Evidence |
|---|---|---|
| 1. Message registers | 4 × 64-bit registers, plus a 4 KiB per-thread IPC buffer; bulk via 0006's grants | MIG payload sizes × traced call mix: 4 words fit 37–50% of calls, 16 words only 2 points more; 512 words fit 92–94% |
| 2. Badges | 64 bits, server-chosen; no-senders delivered by a queue, not as bits | the model: ORing badges into the word loses them when two fire |
| 3. Notification word | 64 bits; bits are summaries beyond that | 0005's and 0014's needs |
| 4. Capability space | two-level radix table, 4 KiB leaves from donated memory, O(1), up to 65 536 slots | `portinfo` on 564 processes: median 22, p99 119, max 49 147 |
| 5. Bare `Send` | dropped; deferred replies through reply objects instead | 172 one-way routines: 165 are halves of deferred-reply routines, 7 truly one-way, 1 fired |

The measurements are new and recorded in `docs/phase1-data/item6/`
(tools in `tools/phase1/`). They answer questions Phase 0's counts couldn't:
how big each message is, how many capabilities a real process holds, and
what the one-way messages are for.

**Two model changes came out of it, both in the model now:**

1. **Queued no-senders** (items 2 and 3). A fired badge account is linked onto
   its endpoint instead of freed, the bound notification gets the binding's
   summary bit, and the owner collects badges with
   `joist_nosenders_next` (O(1), needs RECV; returns 0 when none is queued).
   Pending badges are dropped when the endpoint dies. A badge minted again
   while its previous firing is still queued starts a fresh account, so two
   firings are two events. Binding now needs a badged notification
   capability. New invariants: every fired account is queued exactly once on
   a live endpoint, with no senders; live accounts are never queued.
2. **Reply objects** (item 5). `JOIST_OBJ_REPLY`, created from the server's
   quota; `joist_reply_save` moves a thread's reply capability into one,
   `joist_reply_saved` answers through it from any thread of the space.
   Freeing a bound reply object wakes its caller with `E_PEER`. I6's
   invariant now counts bound reply objects as holders. The per-thread
   reply capability and `ReplyRecv` are unchanged: they stay the fast path.

| Check | Result |
|---|---|
| Unit tests | `test_notify.c` +5 (collect order, 64-bit badges survive, errors, re-fired badge, queue dropped at death; 3 rewritten); `test_ipc.c` +5 (out-of-order deferred replies, reply from another thread, object deleted while bound, reply to a departed caller, errors) |
| Property tests | `test_props_notify.c`'s shadow models the queue and the collect call (about 910 firings and 470 collections per run); `test_props_ipc.c`'s shadow models reply objects (about 1 400 saves, 300 saved replies, 400 stale per run); the whole-model executor gains `collect`, `reply_save` and `reply_saved`, with I6 checked directly on saved replies |
| `make mutate` | 43 of 43 caught, 7 new: a fired badge never queued, collection that doesn't pop, the queue kept at endpoint death, a binding without bits; a saved reply reusable, a freed bound reply object silent, a save overwriting a bound object. The logged run caught 42 and reported one stale ("I4 never fires" targeted the signal line this item replaced); re-targeted, it is caught (`docs/phase1-data/item6/`) |
| `make check` | 76 tests under plain, ASan, UBSan, MSan, TSan and small. Also: `make fuzz-sanity` 7 of 7 from seeds, 7 of 7 from nothing within 26 s; `make fuzz` 10 minutes, 3.7 million steps, no crash |

**Semantics the model chose** (for `docs/abi.md`):

1. **`nosenders_next` returns badge 0 for "none"**, not an error, since 0 is
   never a valid badge.
2. **A reply object needs SEND** to save into or reply through; GRANT lets
   it be passed on, so a thread pool in another task could answer.
3. **Saving into a bound reply object fails** (`E_OCCUPIED`) and leaves the
   thread's reply capability where it was.
4. **Reply objects survive the thread that saved into them**: the reply
   belongs to the space, not the thread.

### Work item 7: `docs/abi.md` (2026-10-03)

`docs/abi.md` is now written from the model's API: objects and the rights
each is created with, rights and badges, and every operation (capability,
task and thread, IPC including deferred replies, notifications, lifetime
notifications) with its arguments, the rights it needs, its result codes in
the order they are checked, whether it blocks, and its bound. It ends with
the result-code table and five items still open before Phase 3: capability
transfer in messages, preemptible cascades, task cycles, revoke and the
batched copy, and thread state and fault IPC.

It describes the model **after** the 0001 amendment was accepted: bare
`Send` is gone (`joist_send` and the `is_call` thread field removed, its
unit tests rewritten as `Call`s, the IPC shadow and the whole-model executor
updated). Verification of that state: 76 tests in six builds, 43 of 43
mutations, 7 of 7 fuzz-sanity bugs, a 5-minute fuzz run with no crash
(`docs/phase1-data/item7/`).

Every check order in `docs/abi.md` is taken from the model's code, and
`tools/phase1/abicheck.py` checks the document against the tests in both
directions: each result code a row lists must be asserted by a unit test or
reached by a property test, and each code a property test reaches must be
listed. The first run found three problems, now fixed: `Recv` listed
`E_GONE`, which it can never return (holding RECV keeps the endpoint alive);
`cap_read`'s row had the wrong checks, and its errors had no test (one
added); and many rows abbreviated their codes ("space", "thread", "as
derive"), which are now spelled out. Result: 139 codes over 21 operations,
all listed, all reached (`docs/phase1-data/item7/abi-coverage.csv`).
`make check` after the added test: 77 tests in six builds.

**Phase 1's exit criterion** (plan §11: invariants proven or property-tested;
the API written down as `docs/abi.md`) is met for the model's scope, with the
open items listed at the end of `docs/abi.md`. Item 8 (TLA+) is optional.

### Work item 8: TLA+ model of IPC racing a preemptible destruction (2026-10-04)

Optional in plan §11; done at the project owner's request when Phase 1 was
accepted. `model/tla/IpcDelete.tla`, checked with TLC (`make tla`).

**What it models.** The host model runs every operation atomically. The
kernel won't: a task's destruction cascades, so 0003 makes it preemptible,
and on SMP other CPUs run in the meantime. The spec takes the model's
semantics for one server task (an endpoint, a reply object, one server
thread) and splits its destruction into the model's own steps
(`joist__space_destroy`): mark the task dead, destroy its thread, then delete
its slots **one per step**. Between any two steps, client threads in another
task call the endpoint (with deferred replies), time out, and pass
capabilities into the server's space. TLC checks every interleaving against:

- **I6**: every waiting caller is named by exactly one live reply
  capability (the server thread's or the reply object's);
- **I10**, safety: nobody is queued on a dead endpoint; liveness: once
  destruction starts, it finishes and every client ends up running, under
  fairness for the destruction steps only;
- **I1**: when destruction ends, the space holds nothing.

**Found: a dying space must refuse capabilities.** With capability transfer
allowed until the space is fully destroyed (`IpcDelete_unsafe.cfg`), TLC
finds an 8-step trace: the sweep deletes slots 1 and 2, a client passes a
capability into slot 2, the sweep finishes, and the capability is left in a
dead space (I1). With transfers refused from the moment the task is marked
dead (`RefuseIntoDying`), every property holds. This becomes a rule for
capability transfer in `docs/abi.md` (Open, items 1 and 2). The host model
can't show it, because its destruction is atomic.

| Check | Result |
|---|---|
| `IpcDelete.cfg` (2 clients, 2 calls each, 3 slots) | no error; 1 460 distinct states, depth 14, invariants and liveness |
| `IpcDelete_large.cfg` (3 clients, 3 calls each, 4 slots) | no error; 143 000 distinct states, depth 25 |
| `IpcDelete_unsafe.cfg` (transfers into a dying space allowed) | `SpaceEmptied` violated, 8-step trace, as intended |
| `make tla-mutate` (`tla/mutate_tla.py`) | 8 of 8 injected bugs caught: a dying server stranding its caller, endpoint death not waking callers, a freed bound reply object stranding its caller, `reply_save` copying, `Recv` while holding a reply capability, a timed-out caller left queued, a destroyed space accepting capabilities, and destruction allowed to stall half-way, which only the liveness property can see |

**Scope and limits.** One server task, one thread, one endpoint and one
reply object; bounded calls. The spec mirrors the model's semantics, written
independently; it doesn't check the C code. Thread spawning into the dying
task isn't modelled: the model refuses it once the task object is dead
(`E_GONE`), which is why marking dead comes first.

**CBMC on the capability space (§8.1).** `model/cbmc/harness_cspace.c`,
`make cbmc`. A **preservation proof** per operation: CBMC starts from *any*
kernel state that `joist_check_invariants` accepts, applies one operation with
arbitrary arguments (out-of-range ones included), and must show every
invariant still holds, I8 directly, and every array access, pointer and shift
in bounds; unwinding assertions turn a too-small loop bound into a failure.
Table sizes are tiny (3 objects, 2 spaces of 2 slots, 1 thread), but within
them this covers every state, not samples.

It found three real defects before passing, all fixed:

1. **The invariant checker read out of bounds on a corrupt state.**
   `joist__check_task` used `task_obj`, `root` and a thread's `obj` as indexes
   without range checks. Reachable states never trigger it; a checker that
   crashes on the corruption it exists to report is still wrong.
2. **`obj_create` reused a slot without clearing it.** Freeing an object
   clears its fields, so reachable states were safe, but a reused slot could
   carry a reply binding into a new reply object (two holders for one call,
   I6). `obj_create` now zeroes the object first, and a new invariant says a
   free object is all zero.
3. **A missing invariant:** a thread holding a reply capability naming
   itself. Impossible (a caller blocks), but the checker accepted it; now it
   doesn't.

| Operation | Result |
|---|---|
| `obj_create`, `cap_derive`, `cap_mint`, grant, `task_create` | verified: 4 061 properties each, 0 failed (3–10 min each) |
| `cap_delete`, `task_destroy` | **not verified**: both can cascade, and the formula exhausts the workstation's 15 GB at every size, split and solver tried (`docs/phase1-data/item8/cbmc-summary.txt`). Covered instead by the property tests, the fuzzer and the TLA+ model of the cascade |

After the fixes: `make check` 77 tests in six builds, `make mutate`
43 of 43 caught.
