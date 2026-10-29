# 0025 — A fallback scheduling context for passive servers

Date: 2026-10-29
Status: **accepted** (the owner, 2026-10-29). The owner chose the approach
(2026-10-29: "mm gets a fallback context"); this decision says what it is.
Revises: `docs/abi.md`, *Scheduling contexts* (`sc_bind`, donation, I11).
Covers: §5.3 (time charged to whoever caused the work), 0009 (faults as
IPC), 0023 (`mm` passive).

## Context

A passive server (no context of its own) runs only on a caller's lent
context. The context goes back to the caller whenever the caller wakes
from its Call, for any reason: the reply, its timeout, an abort, its
death (`docs/abi.md`, donation). If it goes back while the server is still
working on the request, every borrower down the chain "falls back to its
own context", and a passive server has none. It stops where it is: it
can't finish, can't reach its Recv, and every later caller queues on its
endpoint for ever.

`cowstress` (Phase 4 step 7, part E1) found this with `mm`: a client
killed while `mm` ran on its time (a fault or a CLONE). It happened in 28
of 117 kills on 2 CPUs and 37 of 117 on 4. The same follows from a
client's Call timeout, or `thread_abort` or `thread_suspend` of a client
in a Call. The exit run passes today only because `cowstress` lends `mm`
a context of the root's when it sees `mm` stuck.

## Options considered

1. **A fallback context** (this decision, the owner's choice): a context
   bound to a passive thread as its fallback. The thread stays passive.
2. The borrower keeps a dead lender's context until it next receives (a
   change to donation: the context stays lent after its lender is gone).
   It keeps 0023's accounting, but only covers death: a timeout or an
   abort wakes a live lender, which needs its context back.
3. A supervisor (the root, later Phase 9's restarter) that watches for a
   stuck server and lends it time, as `cowstress` does. No kernel change,
   but the server's liveness depends on another task noticing.

## Decision

- **`sc_bind_fallback` (context slot, thread slot):** binds the context
  to the thread as its **fallback**. A thread has its own context or a
  fallback, never both (`E_OCCUPIED`), and at most one fallback. Errors as
  `sc_bind`'s. `sc_unbind` unbinds a fallback too: `E_STATE` while the
  thread runs on it.
- **The thread stays passive.** For donation, a thread with only a
  fallback counts as having no context, so a Call delivered to it lends
  the caller's context as before (§5.3: the server runs at the client's
  priority, on the client's budget).
- **When the fallback runs:** where a borrower would fall back to its own
  context (the loan is taken back from it, or the borrowed context is
  deleted), a thread with a fallback runs on that instead. Only then.
- **When it stops:** when the thread next receives or waits (where a
  borrower gives back what it borrowed), it stops running on its
  fallback. A thread in a Recv runs on nothing, so the next Call delivered
  to it lends it time, as before.
- **A thread on its fallback lends it on** like any context it runs on: if
  it calls another passive server while finishing, the loan comes back to
  it. It does not lend its fallback while idle, since it is never idle on
  it.
- **Never otherwise.** A borrower whose lent context's budget runs out
  waits for that context's next period, as now. The fallback is for time
  taken away, not time used up: a request is still bounded by its
  caller's budget.
- **I11, extended:** a fallback's owner and its thread name each other;
  a thread with a fallback has no context of its own; a thread runs on its
  fallback only while it is serving (not blocked in a receive or wait)
  and holds no loan.
- **`mm` (0023):** the root binds `mm` a fallback when it starts it, at a
  priority at least that of `mm`'s clients and a small budget. The
  fallback only finishes one interrupted request and returns `mm` to its
  Recv.

## Consequences

- `mm` can no longer be stranded by a client that is killed, times out or
  is aborted mid-request. `cowstress` runs without its rescue
  (`COW_RESCUE=0`) and must pass.
- **Accounting:** the rest of an interrupted request is paid by the
  server's fallback, not its caller. §5.3's rule ("time charged to whoever
  caused the work") holds for every request whose caller stays; for one
  whose caller left, the server pays, bounded by one request.
- **Priority:** a server on its fallback runs at the fallback's priority.
  If that is below a queued caller's, the caller waits behind other work
  until the server finishes (no priority inheritance to a fallback). The
  binder chooses the priority; for `mm`, its clients' highest.
- A passive server without a fallback behaves exactly as today.
- **Kernel size:** one field per thread, one call, a branch in three places
  of `sched.c` (reclaim, a deleted context's unwind, giveback on receive).
  No new object type.

## Verification (model first)

- **The model:** the new field and call; unit tests for each way a loan is
  taken back mid-request (the lender killed, its Call timed out, aborted,
  suspended, the lent context deleted), with and without a fallback; the
  thread back on nothing at its next Recv; donation to it as before; I11
  as extended, checked after every step; the property tests' random
  operations include the new call.
- **CBMC** over the new call and the changed paths; **mutations**: the
  fallback taken while idle, kept after the receive, or used when a lent
  budget runs out; donation refused to a thread with a fallback.
- **The kernel:** the ABI call; `mm`'s root binds it; `cowstress` with
  `COW_RESCUE=0` on 1, 2 and 4 CPUs and on the checking kernel; Phase 3's
  suites unchanged.

## Priority check

- **Correctness:** removes a liveness failure of every passive server,
  found by test, with a model-first change.
- **Simplicity:** one field and one call; no new object.
- **Performance:** nothing on the fast path; the branch is taken only when
  a loan is taken back.
- **Reuse:** none affected; Hurd servers are not passive yet.
