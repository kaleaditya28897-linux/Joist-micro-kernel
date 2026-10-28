# 0023 — `mm`: how it runs, how tasks reach it, what it does

Date: 2026-10-28
Status: **draft**, for the owner's acceptance.
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
  | `MM_PROTECT` | first page, pages, permissions | changes the regions' permissions and the mapped pages' (`protect`); a COW page stays read-only until written |
  | `MM_CLONE` | — | makes an empty task (from `mm`'s own quota, O(1) in the kernel, 0011), gives it its own `MM_SLOT` capability, downgrades the caller's writable private pages to read-only (`protect`, one shootdown a run), maps the same pages read-only into the child, and records them as shared. The reply carries a task capability to the child (CONTROL, GRANT): the caller then copies its capabilities with `cap_copy_batch` (a batch stops at `MM_SLOT`, already filled, with `E_OCCUPIED`, and the caller goes on from the slot after it: 0022) and spawns the child's thread |
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
