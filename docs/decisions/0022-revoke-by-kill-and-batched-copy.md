# 0022 — Revoke by killing the object; the capability copy for `fork` in batches

Date: 2026-10-21
Status: **accepted** (2026-10-21), with `docs/phase4-plan.md` (choice 4).
Settles: `docs/abi.md`, open item 4 (revoke and the batched capability
copy, 0003 and 0011), carried from Phase 3.
Covers: 0003 (revoke blocks rather than defers; bounded work), 0011
(`fork`'s capability copy explicit and batched), 0001 (capabilities and
their lifetime notifications), 0020 and the region capability of
`docs/phase4-plan.md` (choice 7).

## Context

Two things Phase 4 needs from the capability layer are missing.

**Revoke.** The memory manager hands out authority it must be able to take
back: a region of a task's addresses it lets the task manage (0008's escape
hatch), and frames. Today a capability is `{object, type, rights, badge}`
and nothing else (`struct joist_cap`, `kernel/include/joist/core.h`). It
records no parent, sibling or generation, so "every copy derived from this
one" can't be found except by scanning every slot of every space
(64 × 8 192 after Phase 4's capacities). The only revocation that exists
is an object's death: an endpoint dies with its last receive right, and
tasks and threads are destroyed. Every capability to the object then fails
with `E_GONE`. That is also Mach's only revocation.

**The batched copy.** 0011 has `fork` copy the parent's capabilities into
the child "in bounded batches", never in one unbounded kernel call. The
Hurd's `fork` recreates the parent's port names in the child
(`docs/client-recovery.md` §4, from glibc's `_Fork.c`), so a copy must keep
slot numbers. Today that takes one `cap_give` per capability.

## Options considered

For revoke:
1. **A derivation tree** (seL4's): every slot links to its derived copies,
   and revoke deletes the descendants, preemptibly. 8 bytes more per slot,
   and every copy, transfer and delete must keep the links, through the
   model, CBMC and TLA+.
2. **Revoke by killing the object** (this decision): the object is marked
   dead, and every capability to it fails at once, as endpoints already do.
3. **A sweep of every slot**, deleting the copies, preemptibly. No new
   record, but every revoke costs O(all slots).

For the copy:
1. **One kernel call per capability** (`cap_give`, today): 2 kernel entries
   per capability for a process with hundreds.
2. **A batched copy that keeps slot numbers** (this decision).
3. **Copying the whole capability space in the kernel.** Unbounded: the
   mistake 0011 rejects.

## Decision

**Revoke by kill, and `cap_copy_batch`.**

- **`obj_kill(slot)`** marks the object a capability names as dead. From
  then on every capability to it, in any space, fails with `E_GONE`. The
  call is O(1). The slots stay occupied until their holders delete them,
  as for dead endpoints (I1 holds unchanged), and the object's quota unit
  returns to its donor when the last capability goes.
- **It needs the kill right**: for frames and regions, the right bit that
  endpoints use for SEND_ONCE (`0x4`), named `JOIST_R_KILL` for those types.
  Boot frames have it. A derive or a transfer can mask it away like any
  right, so `mm` can hand out copies that can't revoke.
- **Built for the objects that need it in Phase 4: frames and regions.**
  - Killing a frame makes it dying, as when its last capability goes
    (0020): no new mapping of it, and the sweep unmaps it everywhere. A
    killed frame can't be lent (0021).
  - **A lent frame can't be killed** (`E_STATE`): its pages are a space's
    page tables, and only that space's death returns them (0021).
  - Killing a region ends its holder's authority over the range at once.
    `mm` then unmaps what it no longer wants with `unmap_range`.
- **No derivation tree in Phase 4.** Mach has no revoke of descendants, and
  the Hurd does not rely on one. It stays a recorded option (option 1) for
  when RS must take back authority it delegated through a server, and is
  reconsidered then.
- **`cap_copy_batch(task, first, count, mask, flags)`**, with
  `count ≤ 32` (the size of one destruction step, `RUN_PREEMPT_SLOTS`):
  - For each non-empty slot `s` in `[first, first + count)` of the
    caller's space, it puts a copy in slot `s` of the task, with rights ∩
    mask and the same badge.
  - It needs CONTROL on the task.
  - It skips capabilities without GRANT, and receive rights unless
    `flags` asks for them (Mach does not inherit receive rights; glibc
    makes new ones). It returns a bitmap of what it skipped.
  - It stops at the first target slot already occupied (`E_OCCUPIED`), or
    a target leaf that doesn't exist (`E_RANGE`; the parent grows the
    child's table first and pays for it), returning how far it got. What it
    copied before stopping stays copied.
  - A dying child fails with `E_DEAD` (TLA+'s rule, `model/tla/IpcDelete.tla`).
  - It never allocates: a copy only counts a reference, and a badge's
    account already exists for any badge already held.
  - It is atomic per call only. If the parent's other threads change its
    capability space during a `fork`, glibc must stop them. Whether Hurd
    glibc does so (`sysdeps/mach/hurd/fork.c`) is checked at the plan's
    step 3.

## Consequences

- `mm` can take back what it delegates, at once and in O(1), without a
  kernel record that grows with copying.
- Revocation is all or nothing per object: there is no "revoke the copies
  made from mine but not mine". Where that is needed, the holder makes a
  fresh object (a new region over the same range) and hands that out
  instead, so it can be killed alone.
- A killed object's quota unit stays charged to its donor until every
  holder deletes its copy. This is the same exposure dead endpoints have
  had since Phase 3, and `no-senders` and dead-name notifications are how
  holders learn to delete.
- A `fork` of a task with *k* capabilities costs ⌈*k*/32⌉ kernel calls
  plus the leaf growth, instead of *k*.
- The model gains `obj_kill` for two object types and `cap_copy_batch`,
  with property tests against a shadow. TLA+'s dying-space rule is
  extended to the batch. Mutations: the copy widening rights, dropping the
  dying-child check, copying past an occupied slot, a kill without the
  kill right.

## Priority check

- **Reliability first:** both calls are bounded (O(1) and 32 slots), and
  neither allocates; nothing grows with how often capabilities are copied.
- **Resilience:** dead objects are announced the way the Hurd already
  expects (`E_GONE`, dead names), so clients' recovery paths see nothing
  new.
- **POSIX:** `fork` keeps the parent's port names, as glibc needs.
- **Reuse:** the semantics stay Mach's (receive-right death, no descendant
  revoke).
- **Performance:** 32 times fewer kernel entries per `fork` for the
  capability copy.
