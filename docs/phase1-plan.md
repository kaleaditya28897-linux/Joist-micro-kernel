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

**Missing, and needed before the step that uses it:**

| Tool | For |
|---|---|
| `clang-tidy`, `scan-build` | §8.1 static analysis on every commit |
| CBMC | §8.1 bounded model checking of capability-space invariants |
| TLA+ tools (`tla2tools.jar`) | the optional TLA+ model |
| `ld.lld`, `llvm-ar`, `llvm-objdump` | §13 toolchain; not needed by the host model, needed from Phase 2 |

These are workstation packages, to be installed by the project owner.

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
