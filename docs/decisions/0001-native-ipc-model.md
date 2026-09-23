# 0001 — Native IPC model

Date: 2026-09-23
Status: proposed
Covers: §6 rows *Port receive right*, *Port send right*, *Send-once right*,
*`mach_msg` RPC*, and *No-senders / dead-name notification*.
Blocks: Phase 3 (IPC and capabilities). Must be accepted before Phase 3 starts.

## Context

The kernel must give the Hurd everything it currently gets from Mach ports,
with bounded cost and no kernel memory that userspace can exhaust. Concretely,
the Hurd relies on four things: transferable capabilities, a request/reply RPC
shape, **no-senders** notifications (so a server can free per-client state when
the client goes away), and **dead-name** notifications (so a client learns its
server died). Hurd-on-L4 stalled largely because L4 provided none of the last
two natively and emulating them in userspace was slow and fragile (§3).

Constraints from the plan: no dynamic allocation in the kernel after boot
(§5.1), bounded worst-case execution time for every syscall (§5.1), and
asynchronous signalling that cannot queue unboundedly (§5.3).

## Options considered

**A — Mach-shaped: asynchronous, kernel-buffered message queues.**
Closest to what Hurd expects, so least userland change. Rejected: kernel-side
queues are exactly the unbounded resource Mach is being replaced for (§2), and
asynchronous delivery gives no natural path for priority inheritance.

**B — Synchronous rendezvous (L4/QNX-shaped) + notification objects.**
`Call` blocks the client until the server replies; the kernel copies directly
between the two threads with no intermediate buffer, so there is nothing to
queue and nothing to account for. Asynchronous events are separate, and are
**bit-sets on a notification object**, never queued messages — a repeated
signal sets a bit that is already set, so memory cannot grow. Lifetime events
(no-senders, dead-name) are delivered through the same notification mechanism.

**C — Hybrid: synchronous fast path with a bounded per-endpoint fallback queue.**
Rejected for now: it reintroduces per-endpoint accounting and a queue-full
policy, which is complexity and a new failure mode for a case (async bulk
messaging) the Hurd does not actually need on the hot path. Recorded on the
"later" list if measurement shows it is needed.

## Decision

Option **B**.

**Primitives.** `Send`, `Recv`, `Call` (send + block for reply), `Reply`,
`ReplyRecv` (reply then immediately wait — the server loop fast path), and
`Wait` on a notification object. Every blocking form takes a timeout (§5.3).

**Capabilities.** A capability names a kernel object and carries rights bits
(send, receive, send-once, grant) plus a server-assigned **badge**. The badge is
what the receiver sees, so one endpoint serves many clients distinguishably —
this is how a Mach receive right with many send rights maps over (§6).

**Reply.** `Call` mints a single-use **reply capability** delivered to the
server. It is consumed on `Reply`. This is the send-once right the MIG stubs
need, and because it is single-use there is no lifetime question to track.

**Transfer.** Short messages travel in registers. Longer payloads are copied
directly thread-to-thread by the kernel, sender to receiver, with no kernel
buffer. Bulk data uses shared memory grants — note this is a **semantics**
change from Mach out-of-line memory, not a drop-in swap (§6), and is the single
largest piece of work in Phase 5.

**Notifications.** A notification object holds a fixed-width bit-set. Signalling
sets a bit; waiting clears and returns them. IRQs, timers, and lifetime events
all arrive this way. Bounded by construction.

**Lifetime events, bounded.** No-senders is reference-counted **per badge**,
with the accounting slots preallocated when the badge is created, not when a
client appears. Dead-name is delivered by invalidating capabilities on endpoint
destruction; holders get an error on next use plus an optional notification.
No allocation happens on the client-arrival or client-death path.

**Scheduling.** The server runs on the client's scheduling context for the
duration of the call (donation), which is what removes the priority inversion
Mach's asynchrony makes unavoidable (§2, §8).

## Open items to settle before Phase 3 coding

These are parameters, not directions; each needs a number and a rationale
recorded here as an amendment.

1. Number and width of message registers.
2. Badge width, and whether badges are kernel- or server-allocated.
3. Notification bit-set width (and what happens when a server needs more
   distinct events than bits).
4. Capability space representation: flat array vs. guarded page table, and its
   effect on the `O(1)` lookup requirement.
5. Whether `Send` (without reply) is needed at all, or whether every
   interaction is a `Call`.

## Consequences

- Easier: no kernel queues to account for, so the §5.1 no-allocation rule and
  the §8 bounded-kernel-memory guarantee hold on the IPC path; priority
  inheritance falls out of donation; the MIG backend has a clean target for
  send-once/reply.
- Harder: every Hurd interaction that assumed fire-and-forget asynchrony must
  become a call or a notification, and out-of-line memory must be re-examined
  per call site rather than mechanically translated (§6).
- Testable: the Phase 1 host-side model can implement exactly these primitives
  and property-test the invariants (no leaked capabilities, notifications never
  lost, bounded memory) before any kernel code exists.

## Priority check

Respects Reliability > Resilience > POSIX > Reuse > Performance. Reliability
drives the choice: option A is the more reusable, more Hurd-compatible design
and is rejected anyway because it cannot bound kernel memory. Reuse loses the
tie, as the priority order requires.
