# 0028 — `mm`: a limit per task tree now, set by the process server later

Date: 2026-10-30
Status: **accepted** (the owner, 2026-10-30)
Revises: 0023's amendment of 2026-10-30 ("no per-client limit until the
process server, Phase 7"). That amendment's reasoning, that the policy
belongs to the process server, is kept. What changes is that the gap is
closed with a default until then. That amendment, and this entry's
draft, placed the process server in Phase 7. §11 brings `proc` up in
Phase 8 (the Hurd boot chain), so the gap would have stayed open longer
than the amendment said.
Covers: §1.1 rank 1 (no unbounded resource usage), §5.1 (mechanism, not
policy), 0023 (`mm`'s records), 0011 (fork).
Blocks: Phase 5 (the interim bound and `MM_SET_LIMIT`); Phase 8 (`proc` sets it).

## Context

Phase 4's exit review found that `mm` limits no client (0023, amendment of
2026-10-30). One task, or one fork tree, can take every page of `mm`'s pool
and every task record (`MM_MAX_TASKS`). After that, every other task's
demand-zero or copy-on-write fault is refused and its thread ends. No
isolation is broken, but the system as a whole stops: a fork bomb in any
program ends every other program's ability to touch new memory.

The amendment left this to the process server in Phase 7, because the
limit is policy and `mm` doesn't know users or sessions. That is right
about the policy. But the process server is `proc`, which §11 brings up
in Phase 8. The gap would stay open for all of Phases 5 to 8, including
Phase 7, the largest phase (3–5 months). The plan ranks
this kind of exhaustion as the thing reliability exists to prevent (§1.1:
"kernel memory exhaustion", and §1's "no unbounded resource usage"). The
kernel already meets that rule; the system around it should too.

What `mm` already knows: each task it serves arrives either by adoption
(`MM_REQ_ADOPT`, from the root, later RS or `proc`) or by a fork of a task
it serves. That is enough to group tasks into **trees**, one per adopted
task, without knowing anything about users.

## Options considered

**A. Keep the gap until the process server** (the 2026-10-30 amendment).
No work now. Exhaustion stays possible through the whole glibc port and
the Hurd's bring-up, and the fix depends on neither slipping.

**B. A fixed ceiling per tree.** One tree can no longer take everything.
But several trees together still can, and the ceiling protects nobody's
minimum.

**C. A reservation and a ceiling per tree, with admission control.** Each
tree is guaranteed a minimum (its reservation) and may grow up to a
ceiling, using pages nobody has reserved. `mm` refuses to adopt a new tree
if the reservations would exceed the pool. This is the QNX/seL4 shape:
partition, then share what's left.

## Decision

**C, with build-time defaults, and an `MM_SET_LIMIT` request for the
process server.**

- **Trees.** An adopted task starts a tree. A forked task joins its
  parent's tree when its fork is confirmed (`MM_CLONE_KEEP`, 0023's
  2026-10-29 amendment), and stays in that tree if the parent dies.
  A pending child counts against the tree from `MM_REQ_CLONE` on, so
  forking without confirming can't escape the ceiling.
  Each task record gains a tree index. Each tree has a page count, a task
  count, a reservation and a ceiling. All of these are fixed-size records,
  sized at build time like the rest of `mm` (0023).
- **Pages.** A fault or request that would take a tree past its ceiling, or
  into pages reserved for other trees, is refused, as a full pool is
  refused today. Under 0027 the refusal is a `PASS` with `SEGV_MAPERR`
  rather than a silent end.
- **Tasks.** A fork that would take a tree past its task ceiling is refused
  with an error the caller sees (`fork` returns `EAGAIN`, as POSIX says for
  `RLIMIT_NPROC`).
- **Admission.** `MM_REQ_ADOPT` fails if the new tree's reservation doesn't
  fit in what remains unreserved. Then the root, or later RS, knows at
  start time, not at the first fault.
- **Defaults** (changeable, build-time): the root's own trees (the servers
  it starts) get a reservation that covers their measured peak. A tree
  started without one gets a small reservation and a ceiling of half the
  pool and half the task records. The values are set from `cowstress`'s and
  the server tests' measured peaks, and recorded with the measurements.
- **`MM_SET_LIMIT`** (root badge only, later `proc`'s) sets a tree's
  reservation and ceilings. It is built with the interim bound in Phase 5,
  so the tests and Phase 7's glibc work can use it. In Phase 8, `proc`
  takes it over. The process server maps
  `RLIMIT_AS` and `RLIMIT_NPROC` and its session policy onto it. `mm` keeps
  the mechanism; the policy moves out, as the 2026-10-30 amendment wanted.
- **Phase 8's exit criterion** gains a line: a fork bomb and a memory hog,
  each in its own session, leave every other session able to fault and
  fork. Phase 5's gains the same test against the build-time defaults,
  with trees in place of sessions. Both are written into §11.

## Consequences

- One misbehaving program, or one fork tree, can no longer stop the
  system. Several can't either, since reservations are never lent.
- `mm` gains a tree record table and two checks on its allocation paths.
  Both are bounded by build-time sizes.
- A reservation is memory nobody else can use while its tree is idle.
  That is the cost of a guarantee, and it is set by whoever adopts the tree.
- `proc`'s work in Phase 8 is smaller: it sets numbers on an existing
  mechanism instead of adding one.

## Verification

- `mmlib`'s unit tests: a tree at its ceiling; a fork past the task
  ceiling; an adoption refused for lack of reservation; a confirmed child
  staying in its tree after its parent dies; a pending child counted; a tree's pages and tasks returned when its last task dies.
- The refinement test's abstract machine gains trees and their counts.
- **Mutations:** the ceiling not checked on the copy-on-write path; a
  child left out of its parent's tree; a pending child not counted; reservations not counted at adoption.
- **The kernel:** a user test runs a fork bomb and a memory hog beside a
  third task that keeps faulting new pages, and that third task must never
  be refused.

## Priority check

- **Reliability:** closes a system-wide exhaustion path now instead of
  after `proc` (Phase 8).
- **Resilience:** a tree's accounts are rebuilt with the rest of `mm`'s
  records (0029).
- **POSIX:** refusals surface as `EAGAIN` from `fork` and as `SIGSEGV`
  from a fault, as on other systems at their limits.
- **Reuse:** none affected.
- **Performance:** two counter checks per allocation.
