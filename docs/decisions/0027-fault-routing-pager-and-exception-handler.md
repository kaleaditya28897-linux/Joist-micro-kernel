# 0027 — Fault routing: a pager for page faults, an exception handler for the rest

Date: 2026-10-30
Status: **accepted** (the owner, 2026-10-30)
Revises: 0009 (one fault handler per thread or task), 0023 (`MM_SLOT` as the
task's fault handler).
Covers: §5.3 (time charged to whoever caused the work), §9.1 (`mm` is
trusted; clients are not), 0007 (page faults as IPC to a pager), 0010
(thread control capability), 0024 (a passive `mm` must not wait on another
server).
Blocks: Phase 7 (glibc's signal thread is written against it).

## Context

Two accepted decisions give a task's fault handler two different owners:

- **0009:** the handler is glibc's signal thread. It gets a thread control
  capability, rewrites the thread's state, and turns the fault into a
  signal (`SIGSEGV`, `SIGILL`, `SIGFPE`, ...).
- **0023:** the handler is `mm`. `MM_SLOT` is the task's fault handler
  (`task_set_fault` with `JOIST_FAULT_NO_THREAD_CAP`), because page faults
  are how demand-zero and copy-on-write work. For anything `mm` can't
  resolve, 0023 says only: "once glibc exists, its own handler gets
  `SIGSEGV`". How is not decided.

The kernel has one handler slot per thread and one per task (0009: the
thread's first, then the task's). With `mm` in that slot, every fault goes to
`mm`, including a divide by zero, which `mm` has no business seeing. And the
only way left to reach glibc is for `mm` to call the task's signal thread.

That is the hazard 0024 found for file servers, in a worse form. `mm` is
one passive thread that every task depends on (§9.1: it reads and writes all
user memory). If it makes a synchronous call into a client's own code,
a hostile or merely hung client stops memory management for the whole
system. Reliability (rank 1) rules that out.

POSIX (rank 3) rules out the other easy answer, never delivering
`SIGSEGV`. Real programs handle it: garbage collectors, JITs, `libsigsegv`,
stack-overflow handlers, test harnesses.

## Options considered

**A. `mm` forwards the fault with a synchronous Call to the task's signal
thread.** No kernel change. Rejected: a trusted, shared server waits on
untrusted code (above).

**B. `mm` forwards asynchronously:** it replies "keep the thread
suspended", signals a notification the task registered, and glibc's
signal thread then fixes the thread up through its own thread capability.
No kernel change. But it needs a second protocol between `mm` and glibc.
glibc must learn *which* thread faulted, and where, from `mm`. And a thread
left suspended by a lost or ignored notification is a silent hang. More
moving parts, spread across two components.

**C. Route by fault kind only:** page faults to `mm`, everything else to
glibc. Small, but a page fault `mm` can't resolve still has nowhere to go
except "terminate". No `SIGSEGV` and no `SIGBUS`: a POSIX break.

**D. Route by fault kind, and let the pager pass a fault on.** Each task has
two fault roles. The kernel tries the pager for page faults. If the pager
declines, the kernel calls the exception handler itself. `mm` replies and
goes back to its Recv; it never calls client code.

## Decision

**D.**

- **Two roles per task.**
  - The **pager**, set with a new `task_set_pager(task, slot, flags)`, at
    task level only. It receives page faults (`JOIST_FAULT_PAGE`) and nothing
    else. `mm` installs `MM_SLOT` here, with `JOIST_FAULT_NO_THREAD_CAP`,
    exactly as 0023 does today with `task_set_fault`.
  - The **exception handler** is today's handler, unchanged (0009: the
    thread's slot first, then the task's). It receives every other fault
    kind, and any page fault the pager passes on. glibc installs its signal
    thread here, with a thread control capability per fault (0010).
- **A new pager reply, `JOIST_FAULT_PASS`.** `r[1]` carries a POSIX code
  the pager chose (`SEGV_MAPERR`, `SEGV_ACCERR`, `BUS_ADRERR`, `BUS_OBJERR`
  for 0024's truncated file or dead file server). The kernel then makes the
  fault Call to the exception handler with the original fault message plus
  that code.
- **At most one pass.** The exception handler's reply is resume or
  terminate. A `PASS` from it means terminate. A fault Call is therefore at
  most two hops, both O(1), and recursion stays excluded (0009).
- **Without a pager,** page faults go to the exception handler, as today.
  Every Phase 3 and Phase 4 test that sets no pager is unchanged.
- **Without an exception handler,** or with a dead one, a passed fault
  terminates the task (0009's rule: a faulting task with no handler stops).
  Until glibc exists, that is exactly what `mm`'s "the thread is ended"
  reply does today.
- **Time:** both hops are charged to the faulting thread's context, like
  any fault Call (0009, §5.3).

## Consequences

- `mm` never waits on a client. Its fault loop stays: resolve and resume,
  or reply `PASS` and go back to Recv.
- `mm` stops seeing faults that aren't about memory (`GP`, `UD`, `MATH`).
- glibc's signal thread is written once, against 0009 and 0010 as already
  designed, for every signal, including `SIGSEGV` and `SIGBUS`.
- **Kernel size:** one slot and one flags byte per task, one call, a branch
  where a fault picks its handler, and one where a fault reply is read. No
  new object type.
- 0023's description of `MM_SLOT` changes from "the task's fault handler"
  to "the task's pager". `mm`'s code changes by one call at adoption and
  one reply value.

## Verification (model first)

- **The model:** the new call and reply. Unit tests for each route: page
  fault with and without a pager; a pass with and without an exception
  handler, and with a dead one; a non-page fault while a pager is set; a
  `PASS` from an exception handler. A new invariant: a thread is in at most
  one fault Call, and a passed fault never returns to the pager. The
  property tests' random operations include the new call.
- **CBMC** over the changed fault paths. **Mutations:** a second pass
  allowed; a page fault sent to the exception handler while a pager is set;
  a non-page fault sent to the pager; the pager's code dropped on the pass.
- **The kernel:** the ABI call. `mm` moves to `task_set_pager`. A user test
  that takes `SIGSEGV`-shaped faults to a handler thread through `mm`'s pass,
  and one that hangs its exception handler and shows `mm` still serving
  other tasks.

## Priority check

- **Reliability:** removes the only path by which a client could stall
  `mm`. Every added step is O(1), with a fixed hop count.
- **Resilience:** `mm` keeps no per-fault state across a pass, so its
  restart (0029) has nothing more to rebuild.
- **POSIX:** `SIGSEGV` and `SIGBUS` reach the program, with their codes.
- **Reuse:** glibc's Hurd signal code keeps the shape 0009 planned for.
- **Performance:** nothing changes for a fault `mm` resolves. A passed
  fault costs one more Call; that path is already slow (it ends in a signal).
