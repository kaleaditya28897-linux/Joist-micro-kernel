# Joist native ABI

**Status: Phase 1 draft, written from the host model** (`model/`, plan §11's
exit deliverable). Every operation below exists in the model with unit,
property and mutation tests (`docs/phase1-plan.md`, *Progress*). This
document fixes the **semantics**: objects, rights, operations, result codes,
blocking and bounds. The machine encoding (which register carries what, the
system-call numbers) is fixed in Phase 3 within the limits of decision 0001's
2026-10-03 amendment (4 message registers, a 4 KiB IPC buffer).

*Status (2026-10-06): unchanged by Phase 2. The Phase 2 kernel exposes only
two temporary debug calls, `debug_write` and `exit` (`kernel/sys/syscall.h`),
which this ABI replaces in Phase 3 (`docs/phase2-exit.md`, *Carried
forward*).*

Invariant numbers I1–I10 refer to `docs/phase1-plan.md`. Model function names
are given as `joist_*` so every statement can be checked against the code.

## Conventions

- **Who acts.** Every operation is made by a thread, and acts in the
  capability space of that thread's task. The model passes the space (`cs`)
  or thread (`t`) explicitly; the kernel takes both from the caller.
- **Capability addresses** are slot numbers in the caller's space, in a
  two-level table (0001 item 4; built in Phase 3 step 3): a root of 64
  entries, each naming a leaf of 128 slots, so slot `s` is entry
  `s / 128`, position `s % 128`; up to 8 192 slots, O(1) lookup. Leaf 0 is
  part of every space; the others are added with `cspace_grow`. **A slot in
  an absent leaf is `E_RANGE`**, exactly as one past the end: no lookup ever
  allocates. (The host model's tests use 2 leaves of 64, `model/Makefile`.)
- **Results.** Every operation returns one result code (below). Checks run in
  the order each table lists, and the first failure is returned **with no
  effect at all**: a failed operation changes nothing (tested by the
  property tests, which compare the whole state after every step).
- **Blocking.** An operation that may block either completes at once, or
  returns `BLOCKED`: the thread then sleeps, and when it wakes (rendezvous,
  timeout, or peer death) its result, message and badge are in its thread
  record. In the kernel, that is the return of the system call.
- **Timeouts** are relative. `0` means "don't block"; `FOREVER` means no
  deadline. Every blocking operation takes one (§5.3).
- **Bounds (I9).** "O(1)" means a fixed number of objects and threads.
  Operations that scan a table are bounded by the table's size; the measured
  worst cases are in `docs/phase1-plan.md`, work item 5. Only operations that
  can destroy objects can cascade, and a cascade is bounded by the tables,
  not per operation (open: see the end).

## Objects

| Type | Created by | Rights it is created with | What it is |
|---|---|---|---|
| Endpoint | `obj_create` | SEND, RECV, GRANT | a synchronous rendezvous point (0001) |
| Notification | `obj_create` | SEND, RECV, GRANT | a 64-bit word of pending bits and a waiter queue (0001, 0005, 0014) |
| Reply object | `obj_create` | SEND, GRANT | holds one saved reply capability: a deferred reply (0001 amendment) |
| Task | `task_create` | CONTROL, GRANT | a capability space, its threads and its memory quota (0004) |
| Thread | `thread_spawn` | CONTROL, GRANT | a thread of a task; its capability is 0010's thread control capability |

**Memory (0003, 0004, I3).** Every object is paid for from a task's quota,
one unit per object; a failed allocation returns `E_NOMEM` and never blocks.
Quota is conserved: the sum of every live task's unspent quota and every
task's charges equals the budget boot gave the root. Destroying an object
refunds its payer. Nothing is allocated when a call arrives or a client dies.

**Task ids.** A task is also named by its id (its capability-space index),
which `task_reap` and `task_descendant` take. An id carries no authority by
itself: those two check ancestry.

**Lifetime.** An object is freed with its last capability (I1). Two
types also *die* before that, after which every use of a remaining capability
fails with `E_GONE`:

- an **endpoint** dies with its last capability that has RECV (the server is
  gone; I5);
- a **task** or **thread** dies when it is destroyed or killed, or when its
  last capability is deleted (a task nobody can name is destroyed, I1).

## Rights

| Right | Bit | Meaning |
|---|---:|---|
| SEND | 1 | `Call` on an endpoint; `Signal` a notification; reply through a reply object |
| RECV | 2 | `Recv` on an endpoint, `Wait` on a notification; owning an endpoint (binding no-senders, collecting badges) |
| SEND_ONCE | 4 | reserved (0001's reply capability is per thread or in a reply object, not a slot) |
| GRANT | 8 | mint a badge; pass the capability to another task |
| CONTROL | = SEND | on a task or thread capability: destroy, kill, spawn into |

**I8: rights never grow.** Every derived, minted or passed capability has a
subset of its source's rights.

## Badges

64-bit values chosen by the server (0001 amendment, item 2). `0` means
unbadged. A badge is set once, by `mint`, from an unbadged capability with
GRANT; it can't be changed or removed afterwards. A receiver sees exactly the
badge of the capability the sender used (**I7**). A server may put anything
in a badge, typically its per-client record's address or index.

## Capability operations

| Operation | Arguments | Results, in check order | Effect | Bound |
|---|---|---|---|---|
| `obj_create` | type, dst | `E_RANGE`/`E_DEAD` (space), `E_RANGE` (dst), `E_TYPE` (not endpoint, notification or reply object), `E_OCCUPIED`, `E_NOMEM` (quota, then object table) | new object, paid by the caller; a capability with all rights in dst | one table scan |
| `cap_derive` | src, dst, mask | `E_RANGE`/`E_DEAD` (space), `E_RANGE` (src or dst), `E_EMPTY`, `E_OCCUPIED` | dst = src with rights ∩ mask; same badge (I8) | O(1) |
| `cap_mint` | src, dst, mask, badge | `E_RANGE`/`E_DEAD` (space), `E_RANGE` (src or dst), `E_EMPTY`, `E_OCCUPIED`, `E_RIGHTS` (src lacks GRANT), `E_BADGE` (src already badged, or badge 0), `E_NOMEM` (no-senders account table full) | as derive, with the badge set (I7); allocates the badge's no-senders account if it is the first sender with it | one account scan |
| `cap_delete` | slot | `E_RANGE`/`E_DEAD` (space), `E_RANGE`, `E_EMPTY` | the capability goes; may fire no-senders, kill an endpoint, free the object, destroy an unreachable task (cascade) | cascade |
| `cap_read` | slot | `E_RANGE` (space or slot past the end), `E_DEAD`, `E_RANGE` (slot in an absent leaf), `E_EMPTY` (the empty slot is still returned) | returns type, rights and badge (inspection) | O(1) |
| `cspace_grow` | leaf index | `E_RANGE`/`E_DEAD` (space), `E_RANGE` (index 0, which is built in, or past the root), `E_OCCUPIED` (already there), `E_NOMEM` (quota, then the leaf pool) | adds the leaf, empty; paid by the caller like an object (one unit, I3) | one pool scan |
| `cspace_shrink` | leaf index | `E_RANGE`/`E_DEAD` (space), `E_RANGE` (index 0, past the root, or absent), `E_STATE` (the leaf still holds a capability) | removes the leaf, refunding its unit; a leaf is never removed with capabilities in it | one leaf |

A space's leaves die with it: task destruction deletes the capabilities in
every leaf, then returns the leaves to the pool and their units to the
parent with the rest of its quota.

Capabilities pass between tasks **in messages** (*Capability transfer*,
under IPC). `joist__cap_grant`, the stand-in the model's tests used before,
is now what a delivery uses for each capability.

## Tasks and threads (0004, 0010, 0013)

| Operation | Arguments | Results, in check order | Effect | Bound |
|---|---|---|---|---|
| boot | budget | `E_STATE` if already booted | the root task holds the whole budget; it has no task object and can't be destroyed (0013) | O(1) |
| `task_create` | donation, dst | `E_RANGE`/`E_DEAD` (space), `E_RANGE` (dst), `E_OCCUPIED`, `E_NOMEM` (quota < donation + 1), `E_NOMEM` (space or object table) | a new task with quota = donation; the caller pays donation + 1 (the task object) and becomes its parent; a task capability in dst | table scans |
| `task_destroy` | task slot | `E_RANGE`/`E_DEAD` (space), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (CONTROL), `E_GONE` (already dead) | its threads die, its capabilities are deleted (cascading), its unspent quota, its live objects' charges and its children go to its parent (children of a parentless space go to the root, so every task keeps an ancestor) (I1, I3) | cascade |
| `thread_spawn` | task slot, dst | `E_RANGE`/`E_DEAD` (space), then the task slot: `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (CONTROL), `E_GONE`; then `E_RANGE` (dst), `E_OCCUPIED`, `E_NOMEM` (caller's quota), `E_NOMEM` (thread or object table) | a running thread in that task, paid by the caller; a thread capability in dst | table scans |
| `task_reap` | task id | `E_RANGE`/`E_DEAD` (space), `E_RANGE` (task id), `E_DEAD` (already destroyed), `E_RIGHTS` (not a strict descendant: itself, the root and fixtures never are) | as `task_destroy`, without a capability to the task: the caller's authority is ancestry (0004 amendment) | cascade |
| `task_descendant` | from | `E_RANGE`/`E_DEAD` (space), `E_RANGE` (from > the table size), `E_EMPTY` (none) | the first live strict descendant whose id is ≥ from | tasks² |
| `thread_kill` | thread slot | `E_RANGE`/`E_DEAD` (space), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (CONTROL), `E_GONE` | the thread dies: dequeued, and a caller it owed a reply wakes with `E_PEER` (I10) | O(1) |

Deleting the last capability to a live thread kills it, as `thread_kill`.

## IPC (0001 and its amendment)

There is **no bare `Send`**: every message is a `Call`, whose reply may be
deferred. A `Call` delivers the message and the sender's badge to a receiver
and hands the receiver a **reply capability** for it. The reply capability is
single-use (**I6**) and, until saved, belongs to the receiving thread.

| Operation | Arguments | Results, in check order | Blocks? | Effect |
|---|---|---|---|---|
| `Call` | endpoint slot, message, timeout | `E_RANGE`/`E_STATE` (thread), `E_TIMEOUT` (timeout 0: a Call always waits), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (SEND), `E_GONE` (endpoint dead), then the message's capabilities (*Capability transfer*) | always: `BLOCKED` | rendezvous with the first queued receiver, or queue (FIFO); then wait for the reply. Wakes with `OK` and the reply, `E_TIMEOUT` (the timeout covers the whole call), `E_GONE` (endpoint died while queued), or `E_PEER` (the reply capability was destroyed unused) |
| `Recv` | endpoint slot, timeout | `E_RANGE`/`E_STATE` (thread), `E_STATE` (holding an unused reply capability: save or answer it first), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (RECV); `E_TIMEOUT` if timeout 0 and no caller, otherwise `BLOCKED`. Never `E_GONE` on return: holding RECV keeps the endpoint alive | if no caller queued | the first queued caller's message and badge, and its reply capability |
| `Reply` | message | `E_RANGE`/`E_STATE` (thread), `E_NOREPLY` (none held), the message's capabilities (the reply capability is kept), `E_PEER` (the caller stopped waiting; the capability is consumed anyway) | no | the caller wakes with `OK` and the message |
| `ReplyRecv` | message, endpoint slot, timeout | `E_RANGE`/`E_STATE` (thread); if a reply capability is held, the message's capabilities (nothing done on failure), then it is used as by `Reply`, its result ignored (a stale caller is not an error here); then as `Recv`: `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (RECV), `E_TIMEOUT` if timeout 0 and no caller, otherwise `BLOCKED` | as `Recv` | the server loop's fast path |
| `reply_save` | reply-object slot | `E_RANGE`/`E_STATE` (thread), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (SEND), `E_NOREPLY` (none held), `E_OCCUPIED` (object already bound) | no | the thread's reply capability moves into the object; the thread may `Recv` again |
| `recv_slots` | up to 2 slots | `E_RANGE`/`E_STATE` (thread), `E_RANGE` (more than 2) | no | where capabilities arriving in this thread's messages go, position by position, until changed (in the kernel: the IPC buffer) |
| `reply_saved` | reply-object slot, message | `E_RANGE`/`E_STATE` (thread), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (SEND), `E_NOREPLY` (object unbound), the message's capabilities (the object stays bound), `E_PEER` (caller stopped waiting; consumed anyway) | no | as `Reply`, from any thread of a task holding the object |

All IPC operations are O(1): queues are linked through thread records.

**Deferred replies** are how a server answers later (`proc_wait`, `select`,
terminal reads) without a thread per waiting client. Reply objects are
created in advance from the server's quota and are reusable after each
reply. If a bound reply object's last capability is deleted, or its task
destroyed, the caller wakes with `E_PEER` (**I10**). A reply object outlives
the thread that saved into it.

**Messages** carry 4 message registers of 64 bits, and up to one 4 KiB page
through the per-thread IPC buffer, which the kernel copies thread to thread;
bulk data uses 0006's shared-memory grants (0001 amendment, item 1). The
model carries the 4 registers.

**Capability transfer** (Phase 3 step 3; closes open item 1). A message
names up to **2** capabilities in the sender's space, each with a rights
mask. *At send* (`Call`, `Reply`, `ReplyRecv`, `reply_saved`) each must be
in range (`E_RANGE`, also for more than 2), present (`E_EMPTY`) and hold
GRANT (`E_RIGHTS`); a failure fails the whole operation with no effect, so a
reply capability stays held and a reply object stays bound. *At delivery*
(when the message reaches the receiver, which for a queued `Call` is when it
is received) capability *i* is copied into the receiver's `recv_slots[i]`
with its rights masked (**I8**), keeping its badge (so it counts as a
sender for no-senders, **I4**). If that can't be done (the receiver offered
no slot for position *i*, the slot is absent or occupied, the sender's
capability is gone or lost GRANT since the send, or the receiving space is
dead, **TLA+'s rule**) that capability is skipped, never an error: the
transfer never blocks, never fails a message the sender was told was fine,
and never allocates (an absent leaf is not grown). The receiver's message
says, position by position, where each capability landed, or
`JOIST_SLOT_NONE`. Bounded: two copies.

**I10, blocking always ends.** A thread blocked in `Call` or `Recv` wakes on
rendezvous, on its deadline, or when its endpoint dies or is freed (`E_GONE`).
A caller waiting for a reply wakes on the reply, its deadline, or the death of
whatever holds its reply capability: the server thread, or the reply object
(`E_PEER`). Teardown of every test sequence checks that nothing is left
blocked except on a reply a live server still owes.

## Notifications (0001, 0005, 0014)

A notification is a 64-bit word of pending bits (0001 amendment, item 3).

| Operation | Arguments | Results, in check order | Blocks? | Effect |
|---|---|---|---|---|
| `Signal` | notification slot | `E_RANGE`/`E_STATE` (thread), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (SEND), `E_BADGE` (unbadged: it would signal nothing) | never | ORs the capability's badge into the word, or hands it to the first waiter |
| `Wait` | notification slot, timeout | `E_RANGE`/`E_STATE` (thread), `E_STATE` (holding an unused reply capability), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (RECV); `E_TIMEOUT` if timeout 0 and the word is 0, otherwise `BLOCKED` | if the word is 0 | returns the word (in the badge field) and clears it |

**I2.** A signal is never lost and never delivered twice: bits clear only
when a `Wait` returns them, and a waiter exists only while the word is 0.
Bits are sticky, which is what closes the futex race in 0014.

**More events than bits.** A bit is then a summary, and the details come from
state that already exists: the no-senders queue (below), or the server's own
records (0005's `select` groups descriptors per bit beyond 64).

## Lifetime notifications (§5.4, 0001 and its amendment)

| Operation | Arguments | Results, in check order | Effect |
|---|---|---|---|
| `bind_nosenders` | endpoint slot, notification slot | `E_RANGE`/`E_DEAD` (space), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (RECV on the endpoint), then the notification: `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (SEND), `E_BADGE` (unbadged) | binds; rebinding replaces; badges already queued stay |
| `nosenders_next` | endpoint slot | `E_RANGE`/`E_DEAD` (space), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (RECV) | returns the oldest queued badge and frees its account, or badge 0 when none is queued |
| `request_deadname` | endpoint slot, notification slot | `E_RANGE`/`E_DEAD` (space), `E_RANGE`, `E_EMPTY`, `E_TYPE` (any capability to the endpoint will do), then the notification: `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (SEND), `E_BADGE` (unbadged), then `E_GONE` (endpoint already dead), `E_NOMEM` (registration table full) | registers |

**No-senders (I4)** is counted per badge, over capabilities with SEND and a
badge, in an account allocated by the `mint` that creates the badge's first
sender (so deleting never allocates). When the last such capability goes and
a notification is bound, the badge is **queued on the endpoint**, linked
through its account (no memory of its own), and the notification is signalled
with the binding capability's badge, a summary bit the server chose. The
server collects badges with `nosenders_next`. Each firing is collected
exactly once; a badge minted again while its last firing is still queued
counts afresh. Unbound endpoints free the account at once. A dead endpoint's
queue is dropped: nobody can collect it.

**Dead names (I5).** When an endpoint dies, every registration on it signals
its notification once, with the registering capability's badge, and is
freed; every remaining capability to the endpoint then fails with `E_GONE`.

## Result codes

| Code | Value | Meaning |
|---|---:|---|
| `OK` | 0 | done |
| `E_RANGE` | 1 | slot, space or thread index out of range |
| `E_EMPTY` | 2 | the slot holds no capability |
| `E_OCCUPIED` | 3 | the destination slot, or reply object, is in use |
| `E_RIGHTS` | 4 | the capability lacks the right the operation needs |
| `E_NOMEM` | 5 | quota or a fixed table is exhausted (never blocks, I3) |
| `E_BADGE` | 6 | badge already set, zero where one is needed, or an unbadged capability where a badge is the signal |
| `E_TYPE` | 7 | wrong object type |
| `E_DEAD` | 8 | the capability space has been destroyed |
| `E_TIMEOUT` | 9 | the deadline passed, or timeout 0 where the operation would block |
| `E_GONE` | 10 | the endpoint, task or thread is dead |
| `E_PEER` | 11 | the other end of a call is gone: the reply capability died unused, or the caller stopped waiting |
| `E_NOREPLY` | 12 | no reply capability to use (I6) |
| `E_STATE` | 13 | the thread isn't running, or holds an unused reply capability |
| `BLOCKED` | 14 | not an error: the thread sleeps until woken |

## Open, before Phase 3 builds on this (as of Phase 1; status updated through Phase 3)

1. ~~**Capability transfer in messages.**~~ Settled in Phase 3 step 3
   (*Capability transfer*, under IPC), with TLA+'s rule (work item 8): a
   transfer into a space whose task is being destroyed does not happen, from
   the moment the task is marked dead. In the model's atomic destruction that
   is "the space is dead"; the kernel's preemptible destruction (item 2)
   must keep it with a dying flag.
2. **Preemptible cascades (I9, 0003).** Destroying a task or deleting a last
   capability may cascade; the work is bounded by the tables (measured ~30×
   the largest non-cascading operation) but not per operation. The kernel's
   version must be preemptible, with the model's work list as its
   continuation. The TLA+ model (`model/tla/IpcDelete.tla`, work item 8)
   checks the model's order under preemption (mark the task dead, destroy
   its threads, then delete one slot per step) against IPC running on other
   CPUs: I6, I10 (including liveness) and I1 hold, **provided** a dying space
   accepts no new capabilities. Without that rule a capability passed in
   after the sweep has gone past its slot leaks, which TLC shows in 8 steps.
3. ~~**Task cycles.**~~ Settled 2026-10-05 (0004 amendment): tasks holding
   each other's task capabilities outlive their last outside capability, and
   any ancestor reclaims them with `task_reap`, found with `task_descendant`.
4. **Revoke and the batched capability copy** (0003, 0011): not yet in the
   model.
5. **Thread state** (register records, TLS base, 0010) and **fault IPC**
   (0009): outside the model's scope until Phase 3.
