# 0004 — Task and thread objects

Date: 2026-09-23
Status: accepted (2026-10-01); amended 2026-10-05 (parent authority over descendants, accepted)
Covers: §6 rows *Task* and *Thread*.

## Context

Mach tasks and threads map onto Joist tasks and threads almost by definition
(§5.2). The decision worth recording is not *whether* they map, but what the
Hurd actually does with them, because that fixes the control operations the
kernel must expose.

**Evidence** (`docs/phase0-data/mach-call-counts.csv`, build workload):
`task_create` 3 064, `thread_create` 7 264, `thread_resume` 11 421,
`task_terminate` 3 059, `task_threads` 2 481, `thread_abort` 2 482,
`task_info` 9 243, `task_get_special_port` 2 302,
`task_set_special_port` 7 085, `thread_set_special_port` 4 783.

Two things stand out. First, roughly **2.4 threads are created per task** —
the Hurd does not use one thread per task, so thread creation is on the hot
path, not a rarity. Second, **special ports are set more often than they are
read** (7 085 vs 2 302 on tasks), which is process setup writing the bootstrap,
exception and auth ports into a fresh task.

## Options considered

**A — Mach-shaped task/thread with the full special-port array.**
Straight port. Keeps `task_get_special_port`/`set_special_port` as kernel
operations with a fixed array of named slots. Simple to port to; but the kernel
grows a table of policy-named slots (bootstrap, exception, auth) that are not
kernel concepts at all.

**B — Task and thread as pure kernel objects; special ports live in the task's
capability space, named by convention.**
The kernel knows about a task's address space, capability space and threads.
It does not know what "the auth port" is. Setting a bootstrap port becomes
installing a capability at a well-known index, which is an ordinary capability
operation, and the naming convention lives in userspace (glibc, proc, RS).

**C — B, but with one kernel-known slot: the fault handler.**
Fault delivery has to be a kernel concept because the kernel raises the fault
(§5.5, decision 0009).

## Decision

**C.**

- A **task** is an address space plus a capability space plus a set of threads.
- A **thread** is a schedulable context belonging to exactly one task, with a
  register state and a scheduling context (§5.6).
- Creating either requires a capability authorising it, and the memory for the
  new object comes from the creator's donated memory (§5.1). A task cannot
  create unbounded threads without paying for them.
- **Special ports are not a kernel concept**, with one exception: each task and
  thread has a kernel-known **fault handler capability**, because the kernel
  is what raises faults. Everything the Hurd calls a special port — bootstrap,
  auth, proc — is a capability installed at a conventional index in the task's
  capability space, and the convention is owned by glibc and `proc`.
- `task_info` and `task_threads` become capability-space enumeration plus a
  small, bounded introspection call. `task_info` fires 9 243 times in the build,
  so it needs to be cheap, but it must also be bounded: it returns a fixed-size
  record, never a variable-length list built in the kernel.
- `thread_abort` (2 482) is the interruption primitive the Hurd's signal
  machinery depends on. It maps to cancelling a thread's in-progress IPC and
  is therefore specified as part of the IPC contract (0001), not as a separate
  thread operation.

## Consequences

- Easier: the kernel's task/thread interface stays small and policy-free, in
  line with §5.1; the Hurd's special-port conventions change from kernel ABI
  into a userspace header, which is far cheaper to evolve.
- Harder: glibc's `sysdeps/joist` must own the index convention and keep it in
  step with `proc`; a mismatch is a confusing failure. Write the convention
  down in `docs/abi.md` when Phase 1 produces it.
- Thread creation being hot (7 264 per build) means the create path needs the
  same care as IPC. Measure it in Phase 3, not Phase 9.

## Priority check

Respects Reliability > Resilience > POSIX > Reuse > Performance. Option A is
the most reusable — the Hurd would need almost no change — and loses because it
puts policy naming inside the kernel, against §5.1. Reuse loses the tie, as the
order requires.

## Amendment, 2026-10-05: parent authority over descendants (accepted)

**Problem, found by the Phase 1 model** (`docs/phase1-plan.md`, work item 5).
A task is destroyed when its last task capability goes. Two tasks holding each
other's task capabilities never lose their last one, so once every outside
capability is gone they live on, holding their quota, with nobody able to
name them. Reference counting can't collect a cycle; the whole-model test
reaches this in about one sequence in ten.

**Options.** (1) seL4's rule: whoever creates a task always keeps a
capability to it. It needs nothing new, but it is a protocol rule nothing
enforces. (2) Parent authority: the model already records each task's parent
(the task that donated its memory, this decision's "memory comes from the
creator"), so let any ancestor destroy a descendant without a capability.
(3) A cycle collector, in the kernel or a privileged task: the most
machinery, and an unbounded traversal (I9).

**Decision: (2)**, by the project owner. Two operations:

- **`task_reap(target)`**: destroy task `target`, exactly as `task_destroy`
  would, if the caller is a strict ancestor of it (`E_RIGHTS` otherwise).
- **`task_descendant(from)`**: the first live descendant whose id is at
  least `from`, so an ancestor can find tasks it holds no capability to.

A task is named here by its **task id** (its capability-space index). The id
alone grants nothing: both operations check the parent chain. The root task
is everyone's ancestor, so nothing that descends from it can be stranded.
When a task dies its children are re-parented to its parent (Phase 1 model),
and a space with no parent hands them to the root, so the chain to the root is
never broken: every task but the root has an ancestor that can reap it.

**Consequences.** `proc` (or any parent) can always reclaim what it started,
which is also what POSIX expects of process trees. The cost is a second
authority path besides capabilities, limited to destruction and to one's own
descendants. Model: `joist_task_reap`, `joist_task_descendant`
(`model/src/task.c`), unit, property (shadow) and mutation tested.
