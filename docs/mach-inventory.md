# Mach dependency inventory

Phase 0 deliverable (§11). Every Mach call the Hurd actually uses, which
component uses it, and the planned mapping onto Joist's native API (§6).

**Status:** method defined, data not yet collected. Collection requires the
testbed (`docs/testbed.md`).

## Why this exists

The plan's central strategy is *Mach-shaped, not Mach-implemented* (§4.2): the
kernel exposes a small native API, and every Mach concept the Hurd uses is
mapped onto it in userspace. That is only safe if we know what the Hurd
actually uses — as opposed to what Mach offers. Mach's interface is large; the
subset the Hurd depends on is much smaller, and the difference is the work we
get to skip.

Phase 0's exit criterion is a decision-log entry for every row in §6. This
file is the evidence behind those entries.

## Method

1. **Static side.** Read the `.defs` files under `gnumach/include/mach/` and
   mark every call *used / unused / used-by-one-thing*. Cross-reference against
   the Hurd tree with `grep` for the generated stub names.
2. **Dynamic side.** Run `rpctrace` on three real workloads — boot to a login
   prompt, an interactive shell session, and a package build — and record which
   calls actually fire and how often. Frequency matters: it orders the Phase 5
   MIG work and the Phase 11 fast-path work.
3. **Reconcile.** A call that is statically referenced but never fires is a
   different risk from one that fires thousands of times per second.

## Table format

| Mach call | Used by | Static / dynamic | Count (boot / shell / build) | Joist mapping | Decision |
|---|---|---|---|---|---|

`Joist mapping` references §6 of the plan. `Decision` is the
`docs/decisions/NNNN` entry that settles it, or `trivial` where §6's row needs
no elaboration.

## Known-hard rows, flagged in advance

These are already known not to be one-line swaps (§6), and should be filled in
first because they size Phase 5:

- **Out-of-line memory → shared memory grant.** A semantics change, not a
  mechanism swap: Mach OOL is copy-on-write with sender-side deallocation. Every
  use site must be examined for whether it depended on copy semantics or merely
  on moving a large buffer cheaply. Expect multi-week work.
- **Port sets → badges on one endpoint.** libports' per-port handlers and its
  ability to move a port between sets at runtime are not obviously the same
  shape as badge demultiplexing. Confirm against real libports usage.
- **`memory_object_*` (external pagers) → page-fault IPC.** Drives the Phase 6
  paging protocol and the libpager rewrite.
- **Exception ports → fault IPC.** Drives glibc signal delivery.

## Open questions

1. Which `.defs` calls are referenced only by `mach-defpager` (which is being
   rewritten anyway) and can therefore be ignored entirely?
2. Which are referenced only by debugging tools rather than the running system?
3. What does the Hurd use that is *not* in the `.defs` files — direct syscalls,
   `mach_msg` idioms, assumptions about message layout?
