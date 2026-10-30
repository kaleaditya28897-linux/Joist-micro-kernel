# 0023 — `mm`: how it runs, how tasks reach it, what it does

Date: 2026-10-28
Status: **accepted** (the owner, 2026-10-28); **amended** 2026-10-28 (how
a passive `mm` collects no-senders, below; accepted by the owner) and
2026-10-29 (`MM_SHARE` by both sides' consent, below; accepted by the
owner).
Settles: `docs/phase4-plan.md`, choice 6 (`servers/mm`) and the order of
work's item 6, as far as the interface and the structure go.
Covers: §5.3 (time charged to whoever caused the work), §9.1 (`mm` in the
trusted base, its authority the frames it is given and control of the
tasks it serves), 0007 and 0009 (faults as IPC), 0008 (`mm` owns the
region map), 0011 (`fork`: an empty task, then `mm`, then the batched
copy), 0020-0022 (page-sized mappings, lent page tables, revoke by kill;
`mm` hands out no frame capability).

## Context

Step 3 measured the fault round trip on the P53
(`docs/phase4-data/step3-fault/`): about 10 500 cycles when the handler has
a scheduling context of its own (each fault changes context twice, and each
change sets the timer), about 2 600 when the handler is passive and runs on
the faulting thread's lent time. The owner chose the second for `mm`
(2026-10-28).

The plan says what `mm` does (anonymous memory, demand-zero, clone with
COW, shared mappings; never a frame capability to a client) but not how a
task reaches it, what it is asked, how it learns a task is gone, or how
its logic is tested. This decision says so, so that the library, its
refinement test and the server can be built against one description.

## Options considered

How `mm` learns that a task it serves is gone:
1. **No-senders** (this decision): each task holds one badged send
   capability to `mm`'s endpoint, the badge naming the task; when the task
   dies its slots are swept, the last capability with that badge goes, and
   the kernel's no-senders notification (I4) tells `mm`. Nothing new in
   the kernel.
2. A death notification on task objects (a new kernel mechanism, like
   dead-name registrations but for tasks).
3. `mm` polling its task capabilities for `E_GONE`. Unbounded latency, work
   per poll proportional to the tasks served.

How a client names the task a request concerns:
1. **By the badge of the capability it calls through** (this decision):
   the badge says who is asking; nothing in the message is trusted to say
   it.
2. By a task capability passed in each message: a transfer per request.

## Decision

**`mm` is one passive server with one endpoint.**

- **Passive** (the owner, 2026-10-28): `mm`'s threads have no scheduling
  context of their own. A fault or a request runs on the client's lent
  time (0009, §5.3) and `mm` answers with ReplyRecv. `mm` does no work of
  its own between requests: no background zeroing, no reclaim. A long
  request (a clone of a large space) runs on, and is bounded by, the
  caller's budget, and is preemptible between kernel calls like any user
  code.
- **One endpoint, one badge per task.** Each task `mm` serves holds exactly
  one capability to it, with SEND and a badge naming the task, in a fixed
  slot (`MM_SLOT`). The same capability is the task's fault handler
  (`task_set_fault` with `JOIST_FAULT_NO_THREAD_CAP`: the badge names the
  task, and no capability is passed per fault) and its way to make
  requests. `mm` keeps, per task, a task capability with CONTROL.
- **A task gone is a badge with no senders.** `mm` binds its endpoint's
  no-senders to a notification of its own. When a served task dies, its
  slots are swept, `MM_SLOT` with them, and the badge's account fires;
  `mm`, woken, collects the badges (`nosenders_next`) and frees that
  task's records and pages. A task never copies its `MM_SLOT` capability
  to anyone (a child gets its own, below), so the badge's last holder is
  the task.
- **Requests** (message registers; `r[0]` the request, the reply's `r[0]`
  the result, a `joist_err`-style code):

  | request | arguments | what `mm` does |
  |---|---|---|
  | `MM_MAP_ANON` | first page, pages, permissions | records a region of anonymous memory: demand-zero, nothing mapped until it is touched. Refused if it overlaps a region |
  | `MM_UNMAP` | first page, pages | forgets that part of the task's regions, unmaps it (`unmap_range`), and drops its pages' references |
  | `MM_PROTECT` | first page, pages, permissions | changes the regions' permissions and the mapped pages' (`protect`); a COW page stays read-only until written. `MM_NONE` (0024): no access, the kernel's mappings removed and the pages kept, each mapped back at its first fault once accessible |
  | `MM_CLONE` | — | makes an empty task (from `mm`'s own quota, O(1) in the kernel, 0011), gives it its own `MM_SLOT` capability, downgrades the caller's writable private pages to read-only (`protect`, one shootdown a run), maps the same pages read-only into the child, and records them as shared. The reply carries a task capability to the child (CONTROL, GRANT): the caller then copies its capabilities with `cap_copy_batch` (0022) and spawns the child's thread. The batch skips `MM_SLOT`: `mm` gives it without GRANT, so the parent's copy never reaches the child, which has its own already (see the correction below) |
  | `MM_SHARE` | first page, pages, the other task (a capability passed in the message), its first page | maps the caller's pages of that range into the other task too, the range becoming a shared region in both. Shared pages are never COW: a later clone of either task maps them into the child as they are, shared with it as well (POSIX: a `MAP_SHARED` mapping stays shared across `fork`) |

- **Faults** (the message of step 3: kind, access, address, pc). A fault in
  a region:
  - **not present, read:** maps a freshly zeroed page with the region's
    permissions, and up to 15 more around it in the same region and
    last-level table that are not yet mapped *(changeable: the plan's 16
    pages)*; they are the task's own, so nothing is COW about them;
  - **not present, write:** maps one freshly zeroed page, with the
    region's permissions;
  - **present, write, page shared by COW:** if it is the last reference,
    makes it writable (`protect`); otherwise copies it into a fresh page
    and maps that in its place (`map_replace`, conditional on the page
    still being the shared one: 0020's Dirty COW guard).
  - **Anything else** (outside every region, a write to a read-only
    region, an execute where not allowed): the thread is ended (the reply
    says so); once glibc exists, its own handler gets `SIGSEGV`.
  Every answer is the fault reply (`JOIST_FAULT_RESUME` or not); the map
  calls before it are `mm`'s own.
- **Records** (compile-time sizes, as the kernel's: a request that would
  overflow one is refused, `E_NOMEM`): per task, its regions (a sorted
  array: first page, pages, permissions, private or shared); per page `mm`
  manages, the frame page that holds it, a reference count and whether it
  is COW-shared. `mm` keeps every frame it is given mapped in its own space
  (read-write, never executable) to zero and copy pages with `memcpy`
  (0020: no kernel copy).
- **Memory:** the root task gives `mm` frames to manage and the page-table
  pages to lend (0021: `mm` lends each new task the pages for its tables
  from its own memory). `mm` hands no frame capability to any task (0022's
  amendment); tasks get memory only through `mm`'s mappings (and, after
  the exit, region capabilities: choice 7).
- **A library and a server.** The logic (regions, the per-page records,
  the fault decisions, clone and share) is `servers/mm/mmlib.c`, calling
  the kernel through a small table of functions. It builds for the host
  against the kernel model as well as for the server. The server
  (`servers/mm/mm.c`) is the ReplyRecv loop around it.
- **The refinement test** (the plan's *Testing*: "COW, refined"): an
  abstract machine in which every task owns an array of page values and a
  clone deep-copies it, run against the concrete one (the kernel model,
  the library, the fault path) on random sequences of map, read, write,
  unmap, protect, share, clone, exit and kill. Every read must equal the
  abstract value; after every step, the library's records must cover the
  kernel's mappings exactly (checked with `map_query`, 0008). Seeded,
  replayable, mutated.

## Consequences

- A fault costs one passive round trip plus `mm`'s map calls (about 2 600
  cycles measured, with one map); a COW copy adds a 4 KiB `memcpy` and a
  `map_replace`.
- `mm` can't prepare pages ahead of time: demand-zero pays its zeroing in
  the fault. If that shows in `cowstress`, a background thread with its own
  context, off the fault path, is the remedy (rejected for now: the owner
  chose passive).
- A clone of a large space runs long on the caller's time, bounded by its
  budget, and preemptible between `mm`'s kernel calls. Its work is
  `mm`'s records walked once plus a `protect` and a `map` per run of
  pages.
- No kernel change: the endpoint, badges, no-senders, the fault path,
  `map_query` and the calls of 0020-0022 are all there.
- `mm` restarting (Phase 9) rebuilds its records from what it holds (its
  task capabilities, its frames) and `map_query`: one pass per frame
  (0020's amendment). Not built now.

## Amendment (2026-10-28, accepted): a passive `mm` collects no-senders lazily

*Decision* says `mm`, "woken", collects the badges of tasks gone. A passive
`mm` (the owner's choice) can't be woken: it has no time of its own to run
on when a notification is signalled. Building the server showed this.

- **`mm` collects at the start of every message** it gets, before looking
  at it: `nosenders_next` until it returns no badge, and for each badge
  `mm_task_gone` and its task capability deleted. The no-senders binding
  stays (without one the kernel frees a badge's account at once and
  queues nothing); its notification is never waited on. The root binds it
  when it starts `mm` (it holds the endpoint with RECV too).
- **No confusion of tasks:** a task's index, and so its badge, is used
  again only after `mm_task_gone`, which only collection calls. A badge
  still queued names the task that held it.
- **No request fails for a dead task's memory:** collection comes before
  everything a message does, so every death queued by then has given its
  pages back before any is allocated.
- **What it costs:** one more system call per message (`nosenders_next`
  finding nothing), and a dead task's pages held until the next message to
  `mm`. The call's cost is measured with the step's exit measurements; if
  it shows, the alternative is to collect only when the pool runs short
  or every Nth message.

Rejected: a thread of `mm` with its own context waiting on the
notification (the owner chose passive: 0023, *Consequences*); the root
relaying deaths to `mm` (`mm` would trust the root for every task's death,
and the root would have to watch every task).

## Amendment (2026-10-29, accepted by the owner): SHARE by both sides' consent

*Decision*'s `MM_SHARE` names the other task by a capability passed in the
message. Building it (part D2) showed that this can't be made safe. A
received capability's `cap_read` gives its type, rights and badge, never
which object it names; so `mm` can't tell which task a task capability in
a message is for. Anyone holding a task capability to any task could mint
it with a misleading badge, and `mm` would map pages into whatever task it
named, or refuse the right one, on the badge's word.

The owner chose instead (2026-10-29), with no kernel change:

- **`MM_REQ_ID`:** a task asks `mm` for its own id, through its own badge.
  The id is the task's index in `mm`'s records and a generation, which
  `mm` bumps each time the index is used again; so an id names one task,
  never a later one with its index. An id carries no authority: it only
  says which task a request means.
- **`MM_REQ_SHARE_ACCEPT`** (sharer's id, first page, pages): the target,
  through its own badge, says it will take that many pages from that task
  at that address. `mm` keeps one pending acceptance per task; a new one
  replaces the old, and one with no pages withdraws it. A new task (an
  index used again) starts with none.
- **`MM_REQ_SHARE`** (first page, pages, the target's id): the sharer,
  through its own badge. `mm` maps only if the target is live, with that
  generation, and has an acceptance pending that names this sharer's id
  and the same page count; it then calls `mm_share` with the address the
  target gave, and the acceptance is used up. Anything else is refused
  (`MM_E_STATE`): no acceptance, another count, another sharer, an id
  whose generation is stale, the target gone.
- **Nothing is mapped into a task that didn't ask for it**, and nothing in
  a message is trusted to say which task is which: both sides are named by
  the badges they call through, as every other request is.

How the two tasks learn each other's ids is theirs to arrange (a parent
learns its child's from `MM_REQ_CLONE`'s reply; anyone can ask for its own
and pass it on). Knowing an id gives nothing without the target's consent.

Rejected: checking the passed capability against `mm`'s own (the kernel
has no call that compares two capabilities' objects, and adding one is a
kernel change for one server's sake); a capability to `mm` per pair of
tasks (unbounded badges).

## Amendment (2026-10-29, accepted by the owner): a fork is confirmed, and an orphan goes with its parent

**The problem** (found by `cowstress`, Phase 4 step 7 part E1):
- **What happens:** a task killed between its `MM_REQ_CLONE` and starting
  its child's thread leaves a child with no thread. Only `mm`'s slot names
  it (and the parent's copy, swept with the parent).
- **Why nothing destroys it:** the child's own `MM_SLOT` holds the last
  sender of its badge, so the badge never goes, and `mm` never forgets it.
- **The same happens** when the reply's task capability can't be placed,
  or when the parent can't start the child.
- **Today:** `cowstress` reaped them by hand, with `task_descendant` and
  `task_reap` from the root (37 in 20 rounds). In general an ancestor
  could reap, but it can't tell such a child from one that runs.

**The change (the owner's choice):**
- **`MM_REQ_CLONE`'s child is pending.** `mm` records its parent's id.
- **`MM_REQ_CLONE_DONE`** (child's id, `MM_CLONE_KEEP` or
  `MM_CLONE_CANCEL`), from the parent, through its own badge:
  - **keep:** the fork finished, so its child's thread runs. The child is
    an ordinary task from then on, and outlives its parent like any other.
  - **cancel:** `mm` destroys the child at once, on the caller's time. For
    a spawn that failed, or a reply whose capability didn't arrive (the id
    is in the reply's registers either way).
  - **Anything else is refused** (`MM_E_STATE`): an id that isn't a
    pending child of the caller's (another task's, a stale generation,
    confirmed or cancelled already), and `MM_E_RANGE` for another
    `how`.
- **A parent collected with a child still pending takes the child with
  it.** `mm` destroys the child in the same collection. Its badge then has
  no sender, and the same loop collects it. A fork that wasn't confirmed
  never happened, the way glibc's `fork` destroys a child it couldn't
  finish (a killed parent just can't do it itself).
- **When to confirm:** only once the child's thread can run (in the
  tests, after the root started it). A confirmation sent earlier would
  leave the same orphan if the parent died before the start.

**Bounds and costs:**
- **The records:** one id per task (`MM_MAX_TASKS`).
- **The work:** a collection destroys at most `MM_MAX_TASKS` children, each
  destruction finished there, on the time of whoever's message ran the
  collection (as `unmake` already does on a refused CLONE).
- **The price:** one more call per fork.

**Rejected:**
- **The child's first message ends its pending:** no extra call, but
  whether a child survives a parent killed mid-fork would depend on timing.
- **CLONE spawns the thread itself:** atomic, but it reworks `CLONE`, and
  `mm` would do more on the caller's time.
- **The ancestor reaps:** it can't tell an orphan from a child that runs.

**Tested:**
- **`mmroot`'s orphans test:**
  - a cancelled fork is destroyed;
  - confirmations of another task's id, a stale generation, the same
    child twice, or a sibling's child are refused;
  - a pending child its parent destroyed leaves no link behind: the task
    that takes its index next survives the parent;
  - a parent exits with one child confirmed and one pending, and the root
    destroys it. The pending child goes with it, and the confirmed one is
    still served, its pages now its alone (writes copy nothing).
- **`cowstress`:** it now expects `mm` to serve exactly the live tasks
  after every step, and no task but `mm` under the root at the end of a
  round. It no longer reaps.

## Correction (2026-10-29, the owner)

- **What the `MM_CLONE` row said:** a `cap_copy_batch` stops at `MM_SLOT`,
  already filled, with `E_OCCUPIED`, and the caller goes on after it.
- **What happens:** `mm` gives each task its `MM_SLOT` capability with SEND
  only, without GRANT, and the batch skips capabilities without GRANT
  (`docs/abi.md`, `cap_copy_batch`). The parent's copy never reaches the
  child, which has its own from `mm`. Found at step 6 (`docs/phase4-plan.md`).
- **The text changes, not the rights:** skipping is the stronger rule,
  since a task can't pass its badge on at all (*Decision*: a task never
  copies its `MM_SLOT` capability). The row now says so.
- **The clients:** `mmclient` and `cowworker` still go on past a stop at
  `MM_SLOT`. No path reaches it today; it is kept, since it is harmless.

## Amendment (2026-10-30, accepted by the owner): no per-client limit in Phase 4

Found by the review of `mm` at Phase 4's exit (`docs/phase4-exit.md`).

- **The gap:** `mm` limits no client.
  - A task may map up to `MM_TASK_PAGES` pages, as many as the whole pool.
  - It may fork until `mm`'s records (`MM_MAX_TASKS`) are full.
  - So one client, or one fork tree, can take every page and every record.
    After that every other client's demand-zero or copy fault is refused,
    and its thread ends.
- **What it is:** denial of service by exhaustion. It is not a breach of
  isolation: no client can read or write another's pages.
- **Decided:** the limit is policy, and is left to the process server
  (Phase 7). It knows users and sessions, and sets `RLIMIT_AS` and
  `RLIMIT_NPROC`. `mm` has no notion of whom a task belongs to, so any
  limit it set now would be replaced.
- **Until then:** the gap is open. Phase 7's plan must give `mm` a limit
  per task or per tree (pages and tasks), set by the process server.
  A fork child's quota (`MM_CHILD_QUOTA`, 32) already bounds a task's
  live children, not its descendants.

## Priority check

- **Reliability first:** `mm`'s records are bounded at build time; a task's
  death reaches `mm` by a mechanism the kernel already has (I4); faults in
  a dying task go nowhere (its fault handler slot is swept).
- **Resilience:** a request can't name another task by mistake: the badge
  says who asks. A client never holds a frame, so `mm` can always take its
  memory back.
- **POSIX:** anonymous memory, `fork` with COW, `mprotect`, shared memory:
  what glibc's `mmap`, `fork` and `mprotect` need.
- **Reuse:** the Hurd's `fork` shape (glibc stops the other threads, the
  parent copies the port names: `_Fork.c`) is unchanged; only who makes
  the child and copies its memory moves to `mm`.
- **Performance:** passive, measured 4 times cheaper per fault than a
  server with its own context.
