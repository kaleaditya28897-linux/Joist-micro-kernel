# Joist native ABI

**Status: written from the host model in Phase 1, implemented in Phase 3** (`model/`, plan §11's
exit deliverable). Every operation below exists in the model with unit,
property and mutation tests (`docs/phase1-plan.md`, *Progress*). This
document fixes the **semantics**: objects, rights, operations, result codes,
blocking and bounds. The machine encoding (which register carries what, the
system-call numbers) is fixed in Phase 3 within the limits of decision 0001's
2026-10-03 amendment (4 message registers, a 4 KiB IPC buffer).

*Status (2026-10-15): implemented. Phase 3 put the model's code into the
kernel as its object layer (`kernel/core/`) and fixed the machine encoding
(*Encoding*, below; `kernel/sys/abi.h`); the Phase 2 debug calls are
calls 1 and 2 of it. The semantics above are still the model's, now the
kernel's own.*

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
  not per operation; the kernel runs it in pieces between which it can be
  interrupted (decision 0016; open item 2, settled, at the end).

## Objects

| Type | Created by | Rights it is created with | What it is |
|---|---|---|---|
| Endpoint | `obj_create` | SEND, RECV, GRANT | a synchronous rendezvous point (0001) |
| Notification | `obj_create` | SEND, RECV, GRANT | a 64-bit word of pending bits and a waiter queue (0001, 0005, 0014) |
| Reply object | `obj_create` | SEND, GRANT | holds one saved reply capability: a deferred reply (0001 amendment) |
| Task | `task_create` | CONTROL, GRANT | a capability space, its threads and its memory quota (0004) |
| Thread | `thread_spawn` | CONTROL, GRANT | a thread of a task; its capability is 0010's thread control capability |
| Scheduling context | `sc_create` | CONTROL, GRANT | a priority, a budget and a period: the time a thread runs on (§5.6) |
| Frame | boot (`boot_frame`), `frame_split` | CONTROL, WRITE, KILL, GRANT | a range of physical pages (0013: memory is handed out as capabilities) |
| Region | `region_create` | CONTROL, MAP, KILL, GRANT | a range of one task's addresses in which the task maps windows of frames itself (0030) |

**Memory (0003, 0004, I3).** Every object is paid for from a task's quota,
one unit per object; a failed allocation returns `E_NOMEM` and never blocks.
Quota is conserved: the sum of every live task's unspent quota and every
task's charges equals the budget boot gave the root. Destroying an object
refunds its payer. Nothing is allocated when a call arrives or a client dies.

**Task ids.** A task is also named by its id (its capability-space index),
which `task_reap` and `task_descendant` take. An id carries no authority by
itself: those two check ancestry.

**Lifetime.** An object is freed with its last capability (I1). Three
types also *die* before that, after which every use of a remaining capability
fails with `E_GONE`:

- an **endpoint** dies with its last capability that has RECV (the server is
  gone; I5);
- a **task** or **thread** dies when it is destroyed or killed, or when its
  last capability is deleted (a task nobody can name is destroyed, I1);
- a **frame** or a **region** dies when a holder of its kill right kills
  it (`obj_kill`, 0022: revoke by kill); a region also dies with its task.

A dead object's slots stay occupied until their holders delete them, and its
quota unit returns to its payer with its last capability.

## Rights

| Right | Bit | Meaning |
|---|---:|---|
| SEND | 1 | `Call` on an endpoint; `Signal` a notification; reply through a reply object |
| RECV | 2 | `Recv` on an endpoint, `Wait` on a notification; owning an endpoint (binding no-senders, collecting badges) |
| SEND_ONCE | 4 | on endpoints, reserved (0001's reply capability is per thread or in a reply object, not a slot) |
| KILL | = SEND_ONCE | on a frame or region capability: `obj_kill` (0022). Boot frames have it; masked away like any right, it makes copies that can't revoke |
| MAP | = RECV | on a region capability: `region_map`, `region_unmap`, `region_protect` (0030). The task a region serves holds it alone |
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
| `cap_give` | task slot, src, dst, mask | as for a task capability with CONTROL, then `E_RANGE` (src or dst), `E_EMPTY` (src), `E_RIGHTS` (src lacks GRANT), `E_OCCUPIED` (dst in the task) | copies the capability into the task's slot dst, rights ∩ mask (I8), badge kept: how a creator hands a new task its first capabilities | O(1) |
| `cap_copy_batch` | task slot, first, count, mask, flags → done, skipped | `E_RANGE`/`E_DEAD` (space), then the task slot: `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (CONTROL), `E_DEAD` (the task is dying or destroyed: a dying space accepts nothing, TLA+'s rule); then `E_RANGE` (count > `JOIST_COPY_BATCH` = 32, the range past the table, or an unknown flag); then, part way, `E_RANGE` (a copy's slot is in a leaf the task doesn't have: grow it first) or `E_OCCUPIED` (a copy's slot is taken) | for `fork` (0011): each capability in slots [first, first + count) goes to **the same slot** of the task, rights ∩ mask (I8), badge kept. Skipped, with bit *i* of `skipped` set for slot first + *i*: capabilities without GRANT, and copies that would carry RECV on an endpoint unless flags has `COPY_RECV` (1) (Mach's fork inherits no receive right). A stop leaves `done` = the slots dealt with before it, and what was copied stays (the caller goes on from first + done); `done` = count on success, 0 on a refusal. Never allocates: each copy counts a reference, and its badge's no-senders account exists already. Atomic per call only | 32 copies |
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
bulk data uses 0006's shared-memory grants (0001 amendment, item 1).

**The IPC buffer** (Phase 3's exit). A thread registers one page of its own
as its buffer (`ipc_buffer`, a page-aligned user address, or 0 for none;
`E_RANGE` otherwise). A message then carries `len` words (0 to 512) from
the start of the sender's buffer, copied by the kernel into the start of
the receiver's at delivery, alongside the registers and capabilities; a
reply the same way back. At the send, `len` over 512, or words with no
buffer registered, are `E_RANGE` with nothing sent (like a bad capability,
the reply capability kept). At delivery the words are read from the
sender's buffer as it is then (a queued sender's included), and if they
can't be placed (the receiver has no buffer, or either page isn't mapped as
it must be: readable by the sender, writable by the receiver) the message
still arrives, with `len` 0 and a *lost* mark: as with a capability that
can't be placed, never an error for either side. Words beyond `len` in the
receiver's buffer are left as they were.

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

## Task destruction (0016; Phase 3 step 3)

Destroying a task (`task_destroy`, `task_reap`, the last capability to it
going, a fault with no handler) is done in three parts, in the order the
TLA+ model verified (`model/tla/IpcDelete.tla`):

1. **Begin**, at once: the task becomes *dying*. Every operation then
   treats it as dead (`E_DEAD`; `E_GONE` through its task capability), and
   nothing can put a capability into it: no transfer, grant or creation
   (TLA+'s rule). Its threads are destroyed, waking whoever waits on them.
2. **Sweep**, one capability slot per unit of work, from the task's own
   cursor. Deleting a slot may make another task unreachable; that task's
   destruction begins at once and is swept first, and the first resumes at
   its cursor afterwards.
3. **Finish**, at once: its capability-table leaves and mappings go, and
   its children, unspent quota and the charges of objects it paid for that
   still live go to its parent.

The sweep is the only part that can be long, and it is **preemptible**: one
operation sweeps at most the kernel's preemption limit (32 slots) and
returns with the rest pending. The operation that started the destruction
returns `OK` either way: its own effect, beginning the destruction, is
done. Meanwhile everything else runs and sees the dying task as dead.

**Who finishes it (Phase 3 step 7).** Every kernel entry, whatever it is
for, sweeps one more piece of what is pending, and an idle kernel sweeps
the rest; so a destruction always ends, and nothing, a wake-up by interrupt
least of all, waits for more than a piece. Whoever wants it done sooner
calls **`continue`** (budget): one more piece of at most `budget` slots,
never more than the kernel's limit whatever is asked (0 asks for the
limit); `OK`, and whether more remains. A caller that needs the memory back
before it goes on loops on it, on its own time.

## Frames and mappings (0013, 0020, 0021, §5.5; Phase 3 step 3, Phase 4 step 2)

The kernel manages only page tables and frame capabilities (§5.5). At boot
it makes **frames** for the free memory and gives them all to the root
task (0013); frames never overlap, so no page is handed out twice. On a
frame capability CONTROL (the SEND bit) allows mapping, splitting and
lending, **WRITE** (the RECV bit) allows mapping writable, so a read-only
frame capability can be passed on, and **KILL** (the SEND_ONCE bit) allows
`obj_kill` (0022). The memory manager never hands its frames to the tasks
it serves, since a kill can't take back the pages while a holder keeps a
dead copy (0022's amendment): they get regions.

A space's page tables are paid for by frames **lent** to it (0021): its
tables, and the kernel's record of them, come from those frames' pages, and
a call that needs a table the space has no page for fails `E_NOMEM`. A lent
frame can't be mapped or split, and is its owner's ordinary frame again
when the space dies. A mapping is any run of a frame's pages (0020): the
record of what is mapped is the page tables themselves, and each frame
counts the page entries naming it. Every call over pages covers at most one
last-level table (`JOIST_MAP_SPAN` = 512 aligned pages) and needs a task
capability with CONTROL to the space it changes, **the caller's own
included**: no call over pages takes `JOIST_SLOT_SELF`. The root task gets
a capability to itself at boot for that (`boot_self`), and the kernel lends
its space a frame for its tables before loading it (`kernel/arch/x86_64/run.h`).

| Operation | Arguments | Results, in check order | Effect | Bound |
|---|---|---|---|---|
| `boot_frame` (kernel, at boot) | dst, first page, pages | `E_RANGE` (space), `E_STATE` (not the root), `E_RANGE` (dst), `E_OCCUPIED`, `E_RANGE` (empty, wrapping, or over 2^32 - 1 pages), `E_STATE` (overlaps a frame), `E_NOMEM` | a frame in the root's slot, paid from its quota | one table scan |
| `boot_self` (kernel, at boot) | dst | `E_RANGE` (space), `E_STATE` (not the root, or it has one), `E_RANGE`/`E_OCCUPIED` (dst), `E_NOMEM` | a task capability with CONTROL to the root's own space; deleting it never destroys the root, and `task_destroy` refuses it (`E_RIGHTS`) | one table scan |
| `frame_split` | frame slot, page, dst | as for a frame capability with CONTROL, then `E_RANGE` (page not inside), `E_RANGE`/`E_OCCUPIED` (dst), `E_STATE` (another capability names it, or it is mapped or lent), `E_NOMEM` | the frame keeps the pages before `page`; a new frame with the rest, same rights, goes in dst, paid by the caller | one table scan |
| `space_donate` | task slot, frame slot | for the frame, then the task (each with CONTROL); then `E_STATE` (the frame is mapped, lent or dying, or its pages can't be reached by the kernel or lie beyond page 2^32 - 2) | the frame is lent to the task's space for its page tables | O(1) |
| `map` | frame slot, page offset, pages, task slot, first virtual page, permissions | for the frame, then the task (each with CONTROL): `E_RANGE`/`E_DEAD`, `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS`, `E_GONE`; then `E_RIGHTS` (writable and executable, an unknown permission, or writable without WRITE), `E_RANGE` (no pages, past the frame, outside the user half, or across a last-level table), `E_STATE` (the frame is lent), `E_OCCUPIED` (a page already mapped), `E_NOMEM` (too few lent pages for the tables it needs) | those pages of the frame appear at those pages of the task, readable, and writable **or** executable (W^X) | the run's pages, and at most three new tables |
| `map_replace` | as `map`, then the old frame's slot and its page offset | as `map`, with the old frame's capability checked third; then `E_STATE` if any page of the run doesn't hold exactly the old frame's pages from that offset, and nothing changes | the run's pages now show the new frame's, with these permissions; the TLBs are flushed | the run's pages |
| `unmap_range` | task slot, first virtual page, pages | as for a task capability with CONTROL, then `E_RANGE` (no pages, outside the user half or across a last-level table) | whatever is mapped there goes; holes are fine; tables left empty go back to the space's lent pages | the run's pages, and the three tables above |
| `obj_kill` | slot | `E_RANGE`/`E_DEAD` (space), `E_RANGE`, `E_EMPTY`, `E_TYPE` (not a frame or a region), `E_RIGHTS` (KILL), `E_GONE` (killed already), `E_STATE` (a frame lent to a space) | revoke by kill (0022): the frame dies, so every capability to it, in any space, fails `E_GONE` (`map`, `frame_split`, `space_donate`, `obj_kill`), and it can't be mapped again; the destruction work list unmaps it everywhere, a last-level table per step (`continue`). Its slots stay until their holders delete them; its pages and its quota unit come back with its last capability. All or nothing per frame: to revoke only what was handed out, hand out a frame of its own | O(1); the unmapping is the sweep's |
| `protect` | task slot, first virtual page, pages, permissions | as `unmap_range`, with `E_RIGHTS` (writable and executable, an unknown permission) before `E_RANGE`; then `E_RIGHTS` if a page is to be writable but was mapped through a capability without WRITE, and nothing changes | the pages mapped there take those permissions; holes are fine | the run's pages |
| `map_query` | task slot, first virtual page, pages (at most 64), frame slot or `J_SLOT_NONE`, the caller's buffer | for the task, then the frame named (each with CONTROL); `E_RANGE` (no pages, more than 64, outside the user half or one `JOIST_MAP_SPAN`, or a buffer that can't take the answer whole); nothing is written on an error | per page, one word: 0 if absent, else present, its permissions, whether its rights ceiling allows writing, and, only for the frame named, that it is that frame and the page offset (0020, amended 2026-10-27). Never another frame, nor a physical address | the run's pages |

The same frame may be mapped in several tasks, or twice in one: sharing is
how memory is passed until 0006's grants (Phase 5). W^X holds per page, not
across aliases of one frame. A frame whose last capability goes while it is
still mapped is **dying**, and a killed one is **dead**, each: it can't be mapped again, stays charged to its
donor, and is unmapped everywhere by the destruction work list, a
last-level table per step (`continue`), then freed; a dying frame that is
lent is freed when its space dies. Destroying a task removes its mappings
the same way. **I12**: frames are disjoint, non-empty and at most 2^32 - 1
pages; every present page entry names a page inside a frame that is not
lent; no page is writable beyond the rights of the capability it was
mapped through, nor writable and executable; each frame's count of the
entries naming it is right; every table of a space lies in a frame lent to
it, and the pages it may still take are counted right; a dying frame has no
capability and is still mapped or lent; a killed frame is not lent. No fault is resolved by the kernel:
a page fault is fault IPC (below), and Phase 4's memory manager maps in
reply.

### Regions (0030; Phase 5 step 3)

A **region** is a range of one task's addresses, whole last-level tables
(at most `JOIST_REGION_TABLES`, 32: 64 MiB), with up to
`JOIST_REGION_EXTENTS` (8) **extents**, each a window of one frame, and a
ceiling (W, X). The memory manager makes it and adds extents; the task,
given a capability with MAP alone, maps any page of any extent anywhere in
the range itself, without a request. Inside a region only these calls act:
`map`, `map_replace`, `unmap_range` and `protect` refuse a run that meets
one (`E_STATE`). A frame in an extent is kept while the region lives, and
can't be split, lent or donated; a lent frame can't be added. Killing a
region (`obj_kill`, KILL) or deleting its last capability unmaps its range
on the destruction work list, a table a step; its task's end ends it too.
A task has at most `JOIST_REGIONS_PER_TASK` (8). **I12** adds: every page
mapped in a region's range is a page of one of its extents, within the
ceiling (and writable only if the extent's frame capability had WRITE); no
extent names a lent frame; outside a dying space no last-level table is
empty.

| Operation | Arguments | Results, in check order | Effect | Bound |
|---|---|---|---|---|
| `region_create` | task slot, first page, tables, ceiling, dst | as for a task capability with CONTROL; `E_RANGE` (dst, an unknown ceiling bit, the first page not on a table, no tables or too many, past the user half), `E_OCCUPIED` (dst), `E_STATE` (a page table or a region already in the range), `E_NOMEM` (quota, objects, records, or the task's 8 regions) | an empty region in dst with every right, paid by the caller | O(tables) |
| `region_add` | region slot, frame slot, page offset, pages | for the region, then the frame (each with CONTROL; `E_GONE` if dead or its task is gone); `E_RANGE` (no pages, past the frame), `E_STATE` (the frame is lent, or the region dying), `E_NOMEM` (8 extents) | the window is the next extent; its index in `RDX` | O(1) |
| `region_map` | region slot, extent, page in it, pages, first virtual page, permissions | for the region (MAP; `E_GONE` also if the extent's frame was killed); `E_RIGHTS` (unknown, W and X, beyond the ceiling, writable beyond the extent), `E_RANGE` (no such extent, past it, outside the region or a last-level table), `E_OCCUPIED`, `E_NOMEM` (no page for a table) | the pages are mapped | the run's pages |
| `region_unmap` | region slot, first virtual page, pages | for the region (MAP); `E_RANGE` (outside it or a table) | whatever is mapped there is unmapped | the run's pages |
| `region_protect` | region slot, first virtual page, pages, permissions | as `region_unmap`, with `E_RIGHTS` (as `region_map`) before `E_RANGE` | the pages mapped there take those permissions | the run's pages |

## Thread control and faults (0009, 0010; Phase 3 step 3)

A **thread control capability** (CONTROL on a thread) is the authority over
a thread; nothing else is checked (0010).

| Operation | Arguments | Results, in check order | Effect | Bound |
|---|---|---|---|---|
| `thread_read_regs` / `thread_write_regs` | thread slot, register record | `E_RANGE`/`E_DEAD` (space), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (CONTROL), `E_GONE` | the thread's register record: one fixed record per architecture (pc, sp, the TLS base, the general registers), never a family of flavours | O(1) |
| `thread_abort` | thread slot | as above | a thread blocked in `Call`, `Recv` or `Wait`, or waiting for a reply, wakes with `E_ABORTED` (glibc's `EINTR`); a caller that stops waiting leaves the server's reply capability stale (its `Reply` gets `E_PEER`). A running or suspended thread is left alone | O(1) |
| `thread_suspend` | thread slot | as above | aborts a blocking operation first, then stops the thread; twice is not an error | O(1) |
| `thread_resume` | thread slot | as above, then `E_STATE` (not suspended) | the thread runs again | O(1) |
| `thread_set_fault` / `task_set_fault` | thread (or task) slot, handler slot, flags | as above for the capability, then `E_RANGE` (handler past the table, or an unknown flag) | names the fault handler: a slot in the thread's (or task's) own space, or none. Nothing is checked until a fault. Flag `NO_THREAD_CAP` (1): page faults to this handler bring no thread capability (Phase 4: the badge names the task, and the reply alone resumes the thread), saving the handler a delete per fault | O(1) |

**The register record in the encoding (Phase 3 step 6).** `thread_read_regs`
and `thread_write_regs` take the thread slot and the address of the record
in the caller's memory: 20 words (pc, sp, flags, the TLS base, then `RAX`
`RBX` `RCX` `RDX` `RSI` `RDI` `RBP` `R8`-`R15` and a spare). The
capability's checks come first; then a record that isn't wholly in the
caller's user memory (writable, for a read) is `E_RANGE` and nothing is
read or written, never a part. Any values may be written: the kernel
trusts none of them and checks on every exit to user mode, for every
thread, that the pc is in the user half, the flags are user flags and the
TLS base is a user address. A thread blocked in a call still gets that
call's outputs when it wakes, over what was written.

**Faults** (0009). A fault in a thread (an exception in user mode; the
kernel's trap handlers raise it, the model's tests call `joist_fault`) is a
`Call` to its handler: the thread's own, else its task's, provided the slot
holds an endpoint capability with SEND whose endpoint is alive. The message
carries, in `r[0]`, the fault kind (page fault, general protection, invalid
opcode, maths, other) in its low byte and, for a page fault, the access it
made in the next (read 1, write 2, execute 4, and 8 if the page was
present: a protection fault; independent of the x86 error code; Phase 4);
in `r[1]` the address, in `r[2]` the error code, in `r[3]` the faulting pc;
and as capability 0 a thread control capability for the faulting thread,
placed in the handler's first receive slot like any transferred capability,
unless the handler was named with `NO_THREAD_CAP` and this is a page fault. The call lends the faulting
thread's scheduling context like any `Call`, so the fault is charged to the
thread that faulted. The handler's reply decides: `r[0] = 0` resumes the
thread (with whatever registers the handler wrote), anything else destroys
it. If the call ends any other way (the handler dies, the reply object is
deleted, the thread is aborted) the fault is unresolved and **the thread is
left suspended**, for its parent or a debugger. **With no usable handler the
task is destroyed** (a faulting system with no handler must stop, not
spin); the root task can't be (0013), so its faulting thread is. A faulting
thread is blocked, so it can't fault again: fault recursion is excluded by
construction.

## Scheduling contexts and donation (§5.3, §5.6; Phase 3 step 3)

A thread runs only on a **scheduling context**: a priority (0–255, higher
first), a budget and a period. A context bound to a thread is its own; a
thread with none is **passive** and runs only on time lent to it. The
kernel's scheduler (Phase 3 step 5) runs threads by their current
context's priority and charges its budget (*Scheduling*, below).

| Operation | Arguments | Results, in check order | Effect | Bound |
|---|---|---|---|---|
| `sc_create` | dst, priority, budget, period | `E_RANGE`/`E_DEAD` (space), `E_RANGE` (dst), `E_OCCUPIED`, `E_RANGE` (budget 0, period 0, or budget > period), `E_RIGHTS` (priority above the space's mcp), `E_NOMEM` (quota, then object table) | a new context, unbound, paid by the caller | one table scan |
| `sc_bind` | context slot, thread slot | for each (context, then thread): `E_RANGE`/`E_DEAD` (space), `E_RANGE`, `E_EMPTY`, `E_TYPE`, `E_RIGHTS` (CONTROL), `E_GONE`; then `E_OCCUPIED` (either already bound) | the context becomes the thread's own; a thread not holding borrowed time starts running on it | O(1) |
| `sc_unbind` | context slot | as above for the context, then `E_STATE` (unbound, lent out right now, or a fallback its thread runs on) | the thread becomes passive (or loses its fallback) | O(1) |
| `sc_bind_fallback` | context slot, thread slot | as `sc_bind`; `E_OCCUPIED` also if the thread has a context of its own or a fallback (0025) | the context becomes the thread's fallback: the thread stays passive, and runs on it only while serving with no loan (a loan taken back mid-request, or woken with nothing) until it next receives or waits; at once if that is now | O(1) |
| `task_set_mcp` | task slot, mcp | as for a task capability with CONTROL, then `E_RIGHTS` (above the caller's own mcp) | the task's mcp: the highest priority it may give a context. The root's is 255, and a new task starts with its creator's | O(1) |

**Donation.** A `Call` delivered to a passive receiver (no context of its
own, holding none) **lends** it the caller's current context: the server
runs at the client's priority, on the client's budget (§5.3), and a server
with no context of its own uses no time unless clients give it. A borrower
that calls another passive server lends the same context on, so a loan can
be a chain. **The context comes back**:

- to the caller, whenever it wakes from the call, for any reason (the
  reply, its timeout, the server's death, a deleted reply object), from
  wherever along the chain it is;
- from the server, when it stops serving: when it receives or waits again
  (so saving a reply for later hands the time back at once), or dies.

**A fallback (0025).** A passive thread may have a fallback context
(`sc_bind_fallback`). It is still passive: a Call delivered to it lends
the caller's context, replacing the fallback. Wherever it would be left
serving on nothing (its loan taken back because the caller died, timed
out or was aborted, or the lent context deleted; or woken from a Recv or
Wait with nothing) it runs on its fallback instead, until it next
receives or waits and blocks. A loan whose budget runs out is not taken
back: the server waits for that context's next period, as before.

Deleting a context unbinds it and unwinds any loan of it; killing a thread
gives back what it borrowed and takes back, then drops, what it lent. **I11**
(checked after every operation): no two threads run on one context; a
context's owner and its thread name each other; every loan is recorded at
both ends, ends at the thread running on the context, never loops, and its
lender is waiting for a reply; a thread has its own context or a fallback,
not both, and runs on its fallback exactly while it serves (is not blocked
in a receive or wait) and holds no loan.

**Scheduling (Phase 3 step 5).** Times are in nanoseconds: budgets,
periods, and the timeouts of `Call`, `Recv` and `Wait` (a timeout is a
length of time from the call, `FOREVER` for none). The rules:

- The thread that runs is the one of highest priority that can run; among
  equals, the one that has waited longest. A thread can run if it is not
  blocked or suspended, runs on a context (its own or a borrowed one), and
  that context has budget left or is due a new period.
- Time is charged to the context a thread runs on, so a server on donated
  time spends its caller's budget. A thread pays for its time in user mode
  and for the kernel's time before it was resumed (from the kernel entry
  that led to it): a call that returns to its caller is the caller's, a
  call that hands over to a server on donated time is the same context's,
  and a preemption is paid by the thread that preempts. No time is free
  but idle time, so a thread that does nothing but make system calls
  still spends its budget. A context whose
  budget is used up runs nothing until its next period, when the budget is
  whole again; a period starts when the context is first used after the
  last one ended. Using a budget up, and waking, put a thread behind its
  equals.
- A context's place in line goes with it (Phase 3 step 8). A server that
  starts running on a caller's context takes the caller's place among its
  equals, and the caller takes it back with the reply: a call to a passive
  server and its answer don't give up the turn, which ends when the budget
  does. (Before step 8 the server went behind its caller's equals.)
- A higher-priority thread that becomes able to run (woken by a message, a
  timeout or a new period) runs at once: the kernel decides again on every
  entry, and its one timer is set for the next moment the answer can
  change without a system call.

A **passive server** needs time once, to reach its first `Recv`. Its
starter binds a context to the server's thread, calls the server once and,
when the answer is back, unbinds the context (`user/root/root.c`). The
answer comes from a `ReplyRecv`, which replies and waits in one step, so
the server is waiting when the starter has it, on any number of CPUs.
(Until step 9 a start-up context of higher priority did the sequencing,
which only works on one CPU.)

**More than one CPU (Phase 3 step 9; decision 0016).** Scheduling is
global: any CPU runs any thread, a thread is on one CPU at a time, and the
rules above hold across them all: the threads running are the best that
can run, a thread that becomes better than one of them takes the CPU of
the lowest, at once (the kernel interrupts that CPU), and an idle CPU
takes whatever can run. There is no affinity to set. Every operation in
this document is atomic as before: the kernel does one at a time, whichever
CPU asks. What that means where one CPU acts on a thread another is
running:

- `thread_suspend`, `thread_kill`, destroying its task, and reading or
  writing its registers take effect before the call returns: the thread is
  off its CPU, its registers in its record. Whatever it was in the middle
  of entering the kernel for at that instant is undone, not lost: a system
  call it had not begun is made again when it next runs, and a fault
  happens again.
- A page unmapped from a task is gone from every CPU before `unmap`
  returns.

The root task is told the number of CPUs in use in `R9` at its start.

**The fast path (Phase 3 step 8)** changes nothing above; it is how the
kernel gets there. After a `Call` that hands the caller's context to a
waiting passive server, or a `ReplyRecv` that hands it back and finds no
next caller, with no timeout on the wait and nothing due, the kernel runs
the other thread at once without asking the scheduler or touching the
timer: the core says when that is certain to be what the scheduler would
choose (`joist_sched_handed`), and a test holds it to that
(`model/test/test_handover.c`). A thread that entered by a system call
leaves by `sysretq`, so `RCX` and `R11` come back clobbered, as *Encoding*
says the `syscall` instruction leaves them; a thread interrupted, new, or
whose registers were written gets every register back.

The root task's own context is the capability in its slot 0: priority 128,
a budget of the whole of a 10 ms period.

## Encoding (x86_64; Phase 3 step 4)

The machine encoding of the operations above (`kernel/sys/abi.h`, which
lists every call number). A thread's user registers are saved in its
register record on every kernel entry and restored from it on every exit;
the dispatcher works on that record alone.

| | Registers |
|---|---|
| In | `RAX` the call number; `RDI`, `RSI`, `RDX`, `R10`, `R8` the arguments, in the order the tables above list them; `R12`-`R15` the four message registers; `R9` the capabilities a message carries |
| Out | `RAX` the result code (0 is `OK`); `RDX` a badge or returned value (the badge of a received message, `Wait`'s bits, `cap_read`'s type and rights, `task_descendant`'s id, `nosenders_next`'s badge, `region_add`'s extent); `R12`-`R15` a received message; `R9` where received capabilities landed; every other register is the caller's (the `syscall` instruction itself clobbers `RCX` and `R11`) |
| `R9` | bits 0-1 the count; per position *i* (0, 1) a 16-bit slot at bit 2 + 20*i* and a 4-bit rights mask at bit 18 + 20*i*; slot `0xffff` is "none" (`JOIST_SLOT_NONE` on receipt; out of range if sent). Bits 48-57 the IPC buffer's word count, sending and received; on receipt bit 63 the *lost* mark |

Calls 10-18 are IPC (`Call`, `Recv`, `Reply`, `ReplyRecv`, `Signal`,
`Wait`, `recv_slots`, `reply_save`, `reply_saved`), 20-29 capabilities and
the table (28 `cap_copy_batch`, 29 `obj_kill`: 0022), 30-38 tasks and
threads, 40-46 frames and mappings
(`frame_split`, `map`, `unmap_range`, `map_replace`, `protect`,
`space_donate`, `map_query`; `map` and `map_replace` take their sixth
argument, the permissions, in `R9`, `map_replace` the old frame and its
offset in `R12` and `R13`, and `map_query` its buffer in `R8`), 50-53
scheduling contexts, 54 `yield` (decision 0014: the calling thread gives up
the rest of its turn and goes behind its equals; its budget is untouched), 55 `sc_bind_fallback` (0025),
56 `clock` (Phase 5 step 7: the time in nanoseconds since boot in `RDX`,
monotonic, as the kernel read it on entry, and the number of CPUs in `R10`;
always `OK`, constant time, for glibc's `clock_gettime`), 60-65 destruction, ancestry and lifetime
notifications, 70-71 a thread's registers (`thread_read_regs`,
`thread_write_regs`), 72 `ipc_buffer`, 80-84 regions (`region_create`,
`region_add`, `region_map`, `region_unmap`, `region_protect`;
`region_create` takes dst in `R8`, `region_add` returns the extent in
`RDX`, `region_map` takes the virtual page in `R8` and the permissions in
`R9`). `thread_spawn` takes the new thread's pc and sp, so it
starts where its creator says; `cap_read` also returns a frame's size in
pages in `R10`; `cap_copy_batch` takes its flags in `R8` and returns `done`
in `RDX` and `skipped` in `R8`. A slot argument wider than 32 bits is out of range, never
truncated into a valid slot. A call that blocks gets its outputs written
when its thread next runs. Call 3 (`debug_counters`, Phase 3's exit) is for the root task only
(`E_RIGHTS` from any other): it writes what the kernel has in use (live
objects, thread records, spaces, table leaves, page entries present,
destructions pending, free page-table frames, free boot frames, the run
queues' insert steps past a list's end so far, and the lent pages the
spaces' tables use) to a record in the
caller's memory (`E_RANGE` if it can't be written whole), for tests that
look for memory the kernel keeps. Calls 1 (`debug_write`) and 2 (`exit`) keep
Phase 2's debug convention, and an unknown number returns `-3` and changes
nothing else.

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
| `E_GONE` | 10 | the endpoint, task, thread or frame is dead |
| `E_PEER` | 11 | the other end of a call is gone: the reply capability died unused, or the caller stopped waiting |
| `E_NOREPLY` | 12 | no reply capability to use (I6) |
| `E_STATE` | 13 | the thread isn't running, or holds an unused reply capability |
| `E_ABORTED` | 14 | the blocking operation was cancelled by `thread_abort` or `thread_suspend` (glibc's `EINTR`) |
| `BLOCKED` | 15 | not an error: the thread sleeps until woken |

## Open, before Phase 3 builds on this (as of Phase 1; status updated through Phase 3)

1. ~~**Capability transfer in messages.**~~ Settled in Phase 3 step 3
   (*Capability transfer*, under IPC), with TLA+'s rule (work item 8): a
   transfer into a space whose task is being destroyed does not happen, from
   the moment the task is marked dead: the space is *dying* (item 2) and
   accepts nothing.
2. ~~**Preemptible cascades (I9, 0003).**~~ Settled in Phase 3 step 3
   (decision 0016, *Task destruction* above): destruction begins at once
   (dying, threads gone), sweeps one slot per unit of work from a per-space
   cursor on a work list, and finishes at once; `continue` resumes it. Every
   intermediate state passes the invariant check, including TLA+'s
   SpaceEmptied, as a C check that no capability sits in a swept slot.
   As written in Phase 1: Destroying a task or deleting a last
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
4. ~~**Revoke and the batched capability copy** (0003, 0011).~~ Settled in
   Phase 4 by decision 0022: revoke by kill (`obj_kill`, *Frames and
   mappings*) and `cap_copy_batch` (*Capability operations*). As written
   before: not yet in the model; moved to Phase 4, which needs them for
   `fork` (`docs/phase3-plan.md`, *Scope*).
5. ~~**Thread state** (register records, TLS base, 0010) and **fault IPC**
   (0009).~~ Settled in Phase 3 step 3 (*Thread control and faults*).
