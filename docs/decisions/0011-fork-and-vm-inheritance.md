# 0011 — `task_create` with VM inheritance → task create + memory manager COW clone

Date: 2026-09-23
Status: proposed
Covers: §6 row *`task_create` with VM inheritance (used by `fork`)*.

## Context

Mach's `task_create` takes an inheritance flag and the kernel builds the child's
address space from the parent's according to per-region inheritance attributes.
This is how the Hurd's `fork` works. §6 maps it onto task create plus a memory
manager copy-on-write clone.

**Evidence** (build workload): `task_create` 3 064, `task_terminate` 3 059,
`thread_create` 7 264, `vm_map` 54 240. Creation and termination tracking each
other almost exactly is a build doing `fork`/`exec`/`exit` several thousand
times. `fork` is a hot path in the workload this project cares most about —
building software.

Note also that the Hurd's `fork` is already unusually expensive: it must
replicate port rights as well as memory, and glibc does substantial work per
`fork` beyond the kernel call.

## Options considered

**A — Kernel-side inheritance, as Mach does it.** The kernel walks the parent's
regions and builds the child's. Puts address-space policy in the kernel
(against §5.7) and makes the kernel's work proportional to the parent's region
count — an unbounded loop over a user-controlled structure, which §5.1
forbids outright.

**B — `mm` clones the address space; the kernel only creates an empty task.**
The kernel call is O(1). `mm` walks its own record of the parent's regions and
installs read-only mappings in both, taking copy faults later (0007).

**C — B plus `posix_spawn` as the preferred path**, so the common
`fork`-then-`exec` case never builds a copy it is about to discard.

## Decision

**C.**

- **The kernel's task creation is O(1)** and creates an empty address space and
  capability space. No inheritance flag, no region walk. This is what keeps the
  §5.1 bounded-WCET rule intact.
- **`mm` implements the clone.** It holds the authoritative region map (0008),
  so it walks its own data, not the kernel's page tables, and installs
  read-only mappings into parent and child. Copy-on-write happens through the
  ordinary fault path (0007, 0009) — no special kernel case.
- **Capability-space copying is explicit and bounded.** `fork` must reproduce
  the parent's send capabilities in the child. This is a loop over the parent's
  capability space, which is user-controlled, so it is **not** a single kernel
  call: it is performed by the caller in bounded batches, or by `mm`/`proc`
  with a budget. A kernel call that copies an arbitrary number of capabilities
  is the same unbounded-loop mistake as option A wearing a different hat.
- **`posix_spawn` is implemented natively**, not as `fork`+`exec`. §9 of the
  plan already calls for this; the measurement is why it matters — several
  thousand `fork`s per build, most of them immediately `exec`ing.

## Consequences

- Easier: kernel stays O(1) and policy-free; COW needs no mechanism beyond the
  fault path that 0007 already requires.
- Harder: `fork` becomes several round trips (create task, clone regions, copy
  capabilities) where Mach had one call plus glibc's port work. It will be
  slower than Mach's `fork` during bring-up. This is an accepted, expected
  regression — §13's risk table already lists "performance worse than GNU Mach
  during bring-up" — and `posix_spawn` is the mitigation for the case that
  dominates.
- The capability-copy loop needs a designed interface, not an afterthought.
  Specify it with the IPC work in Phase 3, because it constrains how capability
  spaces are represented (0001's open item 4).

## Priority check

Respects the order. Option A is faster and much more reusable and is rejected
because it requires an unbounded kernel loop — Reliability beating both
Performance and Reuse, exactly as the order requires.
