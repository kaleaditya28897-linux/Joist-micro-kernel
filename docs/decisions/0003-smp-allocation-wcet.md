# 0003 — SMP, no runtime allocation, and bounded WCET taken together

Date: 2026-09-23
Status: accepted (2026-10-01)
Blocks: Phase 3. These three commitments constrain each other; settling them
late means rediscovering the conflict mid-implementation.

## Context

The plan makes three commitments independently, and they interact:

- **SMP from the design stage** — per-CPU run queues, explicit locking (§5.6).
- **No dynamic allocation in the kernel after boot** — every kernel object is
  paid for from memory donated by a task (§5.1, §8).
- **Bounded worst-case execution time for every syscall** — no unbounded loops
  over user-controlled structures (§5.1, §8).

Each is reasonable alone. Together they remove the standard answers:

1. Per-CPU structures cannot be allocated when a CPU comes online, because
   there is no allocator after boot.
2. Concurrent object deletion (capability revoke, endpoint destroy) normally
   wants deferred reclamation — RCU-style grace periods — but a retire queue
   needs somewhere to put retired objects, which the no-allocation rule denies.
3. Bounded WCET forbids unbounded spinning, so every lock hold time has to be
   bounded *by construction*, not by hoping contention is low.

## Options considered

**A — Relax no-allocation for kernel-internal bookkeeping.**
Keep a small kernel slab for retire queues and per-CPU growth. Rejected: it
reopens the exact hole that makes Mach exhaustible (§2), and "small" is not a
bound anyone can audit.

**B — Preallocate everything per-CPU at boot from a configured CPU maximum,
and bound reclamation with fixed per-CPU retire slots.**
Costs memory proportional to `MAX_CPUS` whether or not those CPUs exist, and
makes hot-plug beyond `MAX_CPUS` impossible. Both acceptable.

**C — Make revoke blocking rather than deferred.**
The caller of a revoke waits until no CPU can still be holding a reference,
rather than queueing the object for later reclamation. Removes the retire queue
entirely, at the cost of a syscall that blocks for a bounded interval.

## Decision

**B and C together.**

- **Per-CPU state is preallocated at boot**, sized from a compile-time
  `JOIST_MAX_CPUS`. Run queues, IPC fast-path scratch, and notification
  accounting slots all come from this. Booting on a machine with more CPUs than
  `JOIST_MAX_CPUS` uses the first `JOIST_MAX_CPUS` and reports the rest as
  unused at boot, loudly. It never silently degrades.
- **Reclamation uses fixed per-CPU retire slots with a bounded drain.** A CPU
  that fills its retire slots drains them before continuing; the drain is
  bounded because the slot count is fixed.
- **Revoke blocks** when the retire path cannot make progress, rather than
  allocating. A bounded wait is acceptable; an unbounded allocation is not.
- **A documented global lock order** is checked in, and no lock may be held
  across an IPC transfer or across any loop whose trip count is user-controlled.
  Violating the order is a review failure, and debug builds assert on it.

## Open items

1. The value of `JOIST_MAX_CPUS`, and the memory cost per CPU at that value.
2. The retire slot count per CPU, and the measured worst-case drain time.
3. Whether the lock-order assertion can be made cheap enough to leave on in
   release builds.
4. Whether revoke's bounded wait is short enough to keep the §8 "deterministic
   IPC path" claim honest, or whether revoke needs to be excluded from that
   claim explicitly.

## Consequences

- Easier: the no-allocation and bounded-memory guarantees survive SMP intact;
  worst-case analysis stays tractable because every structure is fixed-size.
- Harder: memory is spent for CPUs that may not exist; CPU hot-plug beyond the
  configured maximum is not supported; revoke gains a blocking path that must
  be accounted for in the WCET analysis rather than ignored.
- This is the decision most likely to need revisiting once real numbers exist.
  If it does, it is revisited here as an amendment, not silently in code.

## Priority check

Respects Reliability > Resilience > POSIX > Reuse > Performance. Option A is
the faster and simpler implementation and is rejected because it trades the
bounded-memory guarantee, which is Reliability. Performance (a blocking revoke)
loses to Reliability, as the order requires.
