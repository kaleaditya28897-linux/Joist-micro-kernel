# 0009 — Exception ports → fault IPC to a registered handler

Date: 2026-09-23
Status: accepted (2026-10-01)
Covers: §6 row *Exception ports*.

## Context

Mach delivers processor exceptions — page faults that the pager cannot satisfy,
illegal instructions, arithmetic traps — as messages to a thread's or task's
exception port. The Hurd's glibc turns these into POSIX signals in its
per-process signal thread. §6 maps this onto "fault IPC to registered handler",
with the mapping in glibc's signal thread.

**Evidence.** No dedicated exception-port call appears in the trace, because
registration happens through `thread_set_special_port` (4 783 in the build) and
`task_set_special_port` (7 085), which do not distinguish which special port is
being set. The signal machinery's visible footprint is `thread_abort` (2 482)
and `thread_get_state`/`thread_set_state` (4 785 / 7 265) — glibc examining and
rewriting a thread's register state, which is exactly signal delivery and
return. See 0010 for those.

## Decision

- Each task and each thread has a **fault handler capability** (0004). This is
  the one special port the kernel knows about, because the kernel is what
  raises the fault.
- On a fault the kernel **suspends the faulting thread** and performs a
  synchronous `Call` to the handler, carrying the fault type, the faulting
  address where applicable, and a **thread control capability** for the
  faulting thread.
- The handler — glibc's signal thread — inspects and rewrites the faulting
  thread's state through that capability (0010) and replies either *resume* or
  *terminate*. The reply capability is single-use (0001), so a handler cannot
  reply twice and cannot leak the right.
- **Thread-level handler first, task-level as fallback.** If neither is set, or
  the handler capability is dead, the kernel terminates the task. A faulting
  system with no handler must stop, not spin.
- Faults are delivered **synchronously and are charged to the faulting
  thread's scheduling context**. A task that faults constantly pays for it
  itself and cannot starve the system (§5.6, §8).
- Recursion is bounded: a fault raised *by the handler while handling a fault*
  for the same thread terminates the task rather than nesting. Unbounded fault
  recursion is a classic kernel hang and is excluded by construction.

This is deliberately the same shape as the page-fault path in 0007 — a fault is
a `Call` to a capability, whether the answer is a frame or a signal. One
mechanism, two consumers.

## Consequences

- Easier: one fault mechanism serves paging and signals; no exception-message
  format in the kernel beyond a fixed record; priority inheritance applies to
  the handler for free.
- Harder: glibc's signal thread becomes a synchronous server on the fault path,
  so its responsiveness is in the critical path of every fault. Its own faults
  must be impossible or fatal — this is why the recursion rule is stated.
- POSIX signal semantics (`SA_RESTART`, masks across `fork`/`exec`, restarting
  interrupted RPCs) are entirely above this line, in glibc, unchanged in spirit
  from the Hurd's current design. §9 of the plan already commits to keeping the
  Hurd's signal model.

## Priority check

Respects the order. Synchronous delivery costs the faulting thread latency and
buys bounded, accounted, inheritable fault handling — Performance losing to
Reliability, as the order requires.
