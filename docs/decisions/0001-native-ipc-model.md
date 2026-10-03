# 0001 — Native IPC model

Date: 2026-09-23
Status: accepted (2026-10-01)
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

## Amendment, 2026-10-03: the five open parameters (proposed, not yet accepted)

Settled with the Phase 1 model (work items 1–6, `docs/phase1-plan.md`) and
new measurements of the Phase 0 x86_64 traces and the running testbed. Every
number below is in `docs/phase1-data/item6/`, with the tools that produced it
in `tools/phase1/`.

### 1. Message registers: 4 × 64 bits, plus a 4 KiB per-thread IPC buffer

The payload of every routine in the 11 Hurd interfaces the traces touch was
taken from MIG's own generated stubs (`migsizes.py`): fixed fields, variable
fields, and ports (which travel as capabilities, not payload). The variable
parts were sized from the traces (`tracesizes.py`). The share of calls whose
request *and* reply fit in N words, sized conservatively (`regcover.py`):

| Words | boot | shell | build | What arrives at this size |
|---:|---:|---:|---:|---|
| 4 | 46% | 50% | 37% | `io_seek`, the `io_read` request, short replies |
| 16 | 48% | 51% | 39% | almost nothing more |
| 24 | 64% | 84% | 66% | `io_stat`'s 176-byte reply |
| 256 | 93% | 94% | 92% | `dir_lookup`, at MIG's 1 024-byte path maximum |
| 512 | 93% | 94% | 92% | |

- **Registers: 4.** Going from 4 to 16 gains 2 points; the next step needs
  23 words, more than any register file offers. x86_64's syscall convention
  has room for a label word, a capability address and 4 message registers.
- **IPC buffer: one 4 KiB page per thread** (512 words), from the thread's
  donated memory (0004), copied thread to thread by the kernel as the
  decision already says. It holds MIG's inline maxima (a 1 024-byte string,
  2 048 bytes of data) for every hot routine, so MIG's semantics carry over
  unchanged. 63% of the build's 298 280 path names are 16 bytes or longer
  (rpctrace can't show more), so paths belong in the buffer, not registers.
- **Beyond the buffer: shared-memory grants (0006).** What still doesn't fit
  is bulk data: 55% of the build's `io_read` calls ask for more than 4 KiB.

### 2. Badges: 64 bits, chosen by the server

- **Width: 64 bits**, one register; 0 means unbadged.
- **Server-allocated.** Unforgeability comes from who may mint (a GRANT
  holder, I7), not from the badge's value, so the kernel has nothing to gain
  from choosing it. A server-chosen value can be the per-client structure's
  address or index, which is the lookup libports does on every message today.
- **Consequence, found by the model: no-senders can't deliver a badge as
  bits.** The model first ORed the dying client's badge into the bound
  notification's word. With badges as 64-bit values, two clients going at once
  merge into garbage. The model now queues the fired badge on its endpoint,
  linked through its preallocated no-senders account (so the queue needs no
  memory of its own), signals one summary bit the server chose when binding,
  and the server collects the badges with a new O(1) call,
  `nosenders_next`. Unit, property and mutation tested.

### 3. Notification word: 64 bits

One register, returned by `Wait`. When a server needs more distinct events
than bits, a bit is a **summary** and the details come from state that
already exists: the no-senders queue (above), or the server's own records.
Neither dependent decision needs more: 0014's futexes use one notification
per contended lock, so width doesn't matter to them; 0005's `select` maps
descriptors to bits exactly up to 64 and to groups beyond, as 0005 already
says. (`io_select_timeout` fired 7 times in the measured build.)

### 4. Capability space: a two-level table, grown from donated memory

`portinfo` over all 564 processes on the running testbed
(`portcounts.sh`): **median 22 names, 90th percentile 33, 99th percentile
119**, and a few servers far beyond: the root `ext2fs` 49 147, `proc` 2 904,
`auth` 575. Most of `ext2fs`'s names are per-client receive rights (17 462),
which under this decision become badges on a few endpoints, not slots.

- A flat array of fixed size has to be sized to the largest server: rejected.
- seL4-style guarded page tables have variable depth, so lookup isn't
  O(1) by structure: rejected.
- **Chosen: a fixed two-level radix table.** A root page of 512 leaf
  pointers, and 4 KiB leaves of 128 slots, each leaf allocated from the
  task's quota when first used. Lookup is two loads, O(1). A typical task
  needs one leaf; the limit is 65 536 slots, above `ext2fs`'s total even if
  every Mach name were a slot. 0011's batched capability copy for `fork`
  works leaf by leaf. The model keeps a flat array: the ABI sees slot numbers
  only.

### 5. No bare `Send`; deferred replies instead

Of the 172 MIG `simpleroutine`s (one-way messages) in the Hurd's interfaces,
165 are halves of routines (`*_request.defs`, `*_reply.defs`), which exist for
**deferred replies**: `proc` answers `proc_wait` when a child exits, and
`select` and terminal reads wait too. The remaining 7 are truly one-way: four
`*_reauthenticate` variants, `dir_changed`, `file_changed` and
`exec_setexecdata`. Only one fired in any workload: `io_reauthenticate`
(5 792 times in the build).

- In a synchronous kernel, `Send` isn't fire-and-forget anyway: it blocks
  until a receiver arrives. All it saves is the reply.
- `io_reauthenticate` becomes a `Call` whose server replies at once, then
  does the `auth` rendezvous. That costs one empty reply, 0.7% of build calls.
- `dir_changed` and `file_changed` become notifications (a server must never
  block on a slow watcher).
- **Deferred replies are what's actually needed, and the model now has
  them.** A server pre-creates reply objects from its own quota, moves a
  reply capability into one (`reply_save`), keeps receiving, and answers
  later from any of its threads (`reply_saved`). That is single-use (I6),
  allocates nothing when the call arrives, and wakes the caller with
  `E_PEER` if the object goes while bound (I10). It is the shape of seL4's
  MCS reply objects. Unit, property and mutation tested.

**Proposed: the ABI has no `Send`.** The model's `joist_send` stays until
this amendment is accepted, then goes.
