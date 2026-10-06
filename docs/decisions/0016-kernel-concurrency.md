# 0016 — Kernel concurrency: one lock, interrupts off, preemption points

Date: 2026-10-06
Status: proposed
Covers: §5.6 (SMP from the design stage), §5.1 (bounded WCET), 0003 (lock
order, retire slots), `docs/abi.md` open item 2 (preemptible cascades).
Blocks: Phase 3 (step 4 onwards, `docs/phase3-plan.md`).

## Context

Phase 3 puts the model's object layer into the kernel and must run it on
more than one CPU (§5.6, 0003) with bounded worst-case execution time (§5.1).
Two questions have to be answered together:

1. **When can the kernel be interrupted?** Most operations are short and
   bounded by constants. A few are not short: destroying a task, or
   deleting the last capability to one, cascades. The model bounds that work
   by its table sizes (measured at about 30 times the largest non-cascading
   operation, `docs/phase1-exit.md`), not per call. Run with interrupts off,
   a cascade delays every interrupt and every higher-priority thread on that
   CPU by its whole length.
2. **How do CPUs share kernel state?** 0003 already requires a documented
   lock order, no lock held across an IPC transfer or a user-sized loop, and
   preallocated per-CPU state. It does not choose how many locks.

The TLA+ model (`model/tla/IpcDelete.tla`) checked one concrete answer to
the cascade question: mark the task dead, destroy its threads, then delete
one slot per step, with IPC from other CPUs in between. I1, I6 and I10 hold,
liveness included, **provided** a dying space accepts no new capabilities.

## Options considered

**A — Fully preemptible kernel with fine-grained locks** (Linux-style). Any
kernel code may be interrupted; each object has its own lock. Best latency
and scaling in theory. But every operation then needs a locking argument,
the lock order (0003) covers dozens of locks, and the model's sequential
semantics, which every test and proof so far assumes, no longer describe
what runs. The largest correctness cost of any option, for performance
the project ranks last (§1.1).

**B — Non-preemptible kernel, one kernel lock, explicit preemption points**
(the seL4 design). The kernel runs with interrupts off. Every operation is
short except the cascades, and those stop at **preemption points**: after
each bounded unit of work (one slot, one thread) they check for a pending
interrupt and, if there is one, save their place and return, to be resumed
when the thread runs again. Their place is the model's existing work list
(`pending[]`), plus a cursor. On more than one CPU, the whole kernel is
under **one lock**, taken on entry and released on exit. The model's
semantics stay exactly what runs; the lock order is one lock long; the
worst-case delay for an interrupt is the longest unit of work between two
points, not a whole cascade.

**C — Non-preemptible, one lock, no preemption points.** Simplest. Bounded,
since the tables are fixed, but the bound is the whole cascade: about 30
times the next-longest operation, and it grows with the table sizes.

## Decision

**B.**

- The kernel runs with **interrupts off**, on every path.
- Operations that cascade (task destroy, task reap, a last-capability delete
  that destroys a task) have **preemption points**: one slot or one thread per
  unit of work, and a check for a pending interrupt between units. The
  continuation is the model's work list plus a cursor, kept in the kernel's
  fixed state, never on a stack. A preempted cascade is resumed by the next
  system call or event of the thread that started it, and its effects are
  visible as they happen, exactly as the TLA+ model orders them.
- **TLA+'s rule is part of the design**: from the moment a task is marked
  dead, no capability can be transferred into its space.
- On SMP, **one kernel lock**: a ticket lock, taken on every kernel entry
  (system call, interrupt, exception) and released on exit. The lock order of
  0003 is therefore one entry long, and "no lock across an IPC transfer" holds
  because a transfer is a bounded copy of at most 4 KiB under the lock, never
  a wait. Per-CPU state (run queues, the current thread, the idle thread,
  timer deadlines) is reached without the lock where only its CPU touches it.
- **Measured, then revisited.** Phase 3's benchmarks record the lock's
  cost on 1, 2 and 4 CPUs. If a later phase shows contention that matters, it
  is changed by a new entry, not in code.

## Consequences

- Easier: the code that runs is the code the model, its property tests,
  CBMC and TLA+ describe; there are no lock-ordering bugs to find because
  there is one lock; interrupt latency is bounded by the longest unit of work
  between preemption points, which can be measured and is small.
- Easier: 0003's open items 1 and 3 become simple. The lock-order assertion
  is free (one lock), and per-CPU cost is the run queue and a stack.
- Harder: kernel work does not run in parallel on several CPUs. For a
  microkernel whose operations take hundreds of cycles, this has been shown
  acceptable to a few dozen CPUs (seL4's measurements). Joist's §5.8 targets
  are small machines. If it is not, this decision is revisited.
- Harder: every cascade must be written as resumable, and every caller of a
  space must handle "this space is dying". The model already has both.
- The IPC fast path runs under the lock too; it is short enough.

## Priority check

Respects Reliability > Resilience > POSIX > Reuse > Performance. Reliability
decides: B keeps the verified semantics and removes a class of concurrency
bugs, at a cost only to parallel kernel throughput, which is Performance and
ranked last. C would be simpler still but makes the worst-case interrupt
latency scale with table sizes, which §5.1's bounded-WCET commitment rules
out for a real-time-capable kernel (§5.6).
