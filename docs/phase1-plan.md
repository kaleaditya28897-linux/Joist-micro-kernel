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
