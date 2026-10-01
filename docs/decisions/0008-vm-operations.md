# 0008 — `vm_allocate` / `vm_map` / `vm_protect` → memory manager RPCs

Date: 2026-09-23
Status: accepted (2026-10-01)
Covers: §6 row *`vm_allocate`, `vm_map`, `vm_protect`*.

## Context

§6 maps the Mach VM calls onto memory manager RPCs, with the mapping living in
glibc's sysdeps. The inventory shows this is not a cold path.

**Evidence** (`docs/phase0-data/mach-call-counts.csv`, build workload):

| Call | boot | shell | build |
|---|---:|---:|---:|
| `vm_deallocate` | 49 | 241 | **55 580** |
| `vm_map` | 137 | 381 | **54 240** |
| `vm_protect` | 57 | 213 | **30 138** |
| `vm_allocate` | 30 | 171 | **11 957** |
| `vm_region` | 3 | 23 | 2 302 |
| `vm_get_size_limit` | 3 | 23 | 2 302 |

Over 150 000 VM operations in a single package build that does not get past
`configure`. This is the second-hottest family in the trace after port
operations. Note `vm_deallocate` slightly exceeding `vm_map`: this is
`malloc`/`free` and shared-library mapping churn, not a few large allocations.

## Options considered

**A — Keep VM operations as kernel calls.** Fastest, and puts the address-space
policy that §5.7 explicitly excludes back into the kernel.

**B — Every VM operation is an RPC to `servers/mm`.** Clean, and at 150 000
operations per partial build it puts an IPC round trip on the `malloc` path.

**C — Split by whether the operation needs policy.** The kernel owns the page
tables and exposes only mechanical map/unmap/protect of a frame capability
into a range. `mm` owns the address-space *layout* decisions — where an
anonymous region goes, what is reserved, what is copy-on-write. glibc calls
`mm`; `mm` calls the kernel.

## Decision

**C**, with an explicit performance escape hatch.

- **Kernel:** `map`, `unmap`, `protect` on an explicit range with an explicit
  frame capability. No placement policy, no "find me somewhere", no
  zero-fill-on-demand decision. These are the operations that must be bounded
  and are easy to make so.
- **`mm`:** owns the address-space map, placement, anonymous memory,
  copy-on-write, and the `mmap`/`munmap`/`mprotect` POSIX surface. This is
  where §5.7's "paging policy" lives.
- **The escape hatch:** because `vm_deallocate`/`vm_map` dominate, `mm` may
  delegate a **pre-authorised address range** to a task, letting the task
  perform map/unmap within that range by direct kernel call without a round
  trip through `mm`. This is what makes a userspace `malloc` viable. The
  delegation is a capability over a range, so it is still accounted and still
  revocable; it is not a hole in the model.
- The escape hatch is **designed in Phase 4 and measured before Phase 7**,
  because glibc's allocator is built on it. Do not defer it to Phase 11: by
  then glibc will be written against whatever exists.

## Consequences

- Easier: the kernel keeps only page-table mechanics, satisfying §5.7; `mm` is
  free to implement POSIX VM semantics without kernel changes.
- Harder: two places can change a task's mappings (the task within its
  delegated range, `mm` everywhere), so the invariant "`mm`'s view matches the
  kernel's page tables" needs stating precisely and testing. This is a
  property the Phase 1 host model should check.
- The 150 000-operation figure is from a build that stalls at `configure`; the
  real number for a full build is higher. Treat the VM path as hot.

## Priority check

Respects the order. Option A is fastest and loses on §5.7 policy grounds.
The escape hatch is Performance influencing the design, but it is expressed as
a capability over a range — bounded, accounted, revocable — so Reliability is
preserved rather than traded.
