# 0024 — `mm`: guard pages, and regions backed by a file

Date: 2026-10-29
Status: **accepted** (the owner, 2026-10-29), with two changes from the
draft:
- **Protecting to `MM_NONE` keeps the pages' contents**, as POSIX's
  `mprotect` does, instead of freeing them. See *Accepted* below.
- **A passive `mm` and a file server:** option 1, a helper thread.
`MM_NONE` is built (Phase 4 step 7, part E8). File-backed regions are
Phase 6's.
(Drafted at Phase 4's exit, item 7 of `docs/phase4-plan.md`'s order of
work.)
Settles, if accepted: the plan's item 7, "the guard pages" and "the
file-backed rule written down".
Covers: 0007 (external memory objects as page-fault IPC), 0008 (`mm` owns
the region map), 0023 (`mm`'s interface, passive), the plan's choice 8 (a
file-backed mapping whose pager dies).

## Context

`mm` serves anonymous memory only. Two things glibc will need before
Phase 7 are not settled: how a thread's stack gets a guard page, and what
changes when a region's pages come from a file server instead of zeroes.

## 1. Guard pages

**What `mm` does today, for free.** A fault outside every region ends the
thread (0023, *Faults*: "anything else ... the thread is ended"); `mm`
grows nothing (it has no grows-down regions). So a page below a stack that
lies in no region already is a guard page: a fault there ends the thread
and maps nothing. `mmroot`'s "outside" case tests exactly that.

**Why that isn't enough.** A gap is only a guard while nothing else is put
there: a later `MM_MAP_ANON` at that address is accepted (it overlaps no
region), and the guard is gone without anyone asking for it. And `mm` has
no way to say "reserved, no access": a region with neither `MM_W` nor
`MM_X` is *readable* (mmlib.h), which is `PROT_READ`, not `PROT_NONE`.

**Proposed:**

- A permission **`MM_NONE`** (a region that may not be touched at all):
  `MM_MAP_ANON` and `MM_PROTECT` take it; `mm_fault` refuses every access
  in it (the thread is ended, later `SIGSEGV`); nothing is ever mapped in
  it, and protecting a mapped range to `MM_NONE` unmaps its pages (the
  kernel has no "present, no access" entry: x86 can't express it, and a
  read-only mapping would let reads through).
- A guard page is then a region of `MM_NONE`, one page (or more) below the
  stack, made with the stack. This is what glibc does already: nptl's
  `allocate_stack` maps a new stack `PROT_NONE` whole and then
  `mprotect`s all but the guard to read-write, so the one permission gives
  both `PROT_NONE` and guard pages, with nothing stack-specific in `mm`.
- It costs one region entry per stack (16 per task now: a task with many
  threads may need `MM_REGIONS` raised).
- Rejected: a stack-specific region kind that grows down on a fault
  (`MAP_GROWSDOWN`): glibc's threads don't use it, and growing on a fault
  is how a stack runs into the next mapping unnoticed.

## 2. Regions backed by a file

**What changes** when a region's pages come from a file server (0007's
external memory object, reached by IPC) rather than zeroes:

- **Where a page comes from.** A not-present fault in a file region asks
  the file server for that page of the file, and `mm` puts the answer into
  a page of its own pool. `mm` still hands no frame to anyone (0022's
  amendment): the data travels in a message, 4 KiB in the IPC buffer
  (`J_MSG_WORDS`), and `mm` can make the destination pool page its own IPC
  buffer for that call so the kernel's copy lands in place (no `memcpy`).
  No read-around beyond what the server sends: the 16-page read-around of
  demand-zero becomes "as many pages as one request returns", a protocol
  choice.
- **Private file mappings** (`MAP_PRIVATE`): the file's page, once read, is
  shared by every mapper that maps that page of that file privately, COW
  as `mm` does now; `mm` needs a per-file page cache (file, offset → its
  page), which it does not have. A write copies, and the copy is anonymous
  from then on.
- **Shared file mappings** (`MAP_SHARED`): writes must reach the file
  server. `mm` maps the page read-only first and marks it dirty at the
  first write fault (`map_query` shows presence and permissions, not the
  hardware's dirty bit), and writes dirty pages back at `msync`, unmap,
  and before it reclaims one. Write-back is a message to the server, with
  the page in the buffer.
- **Truncation and the pager's death** (plan choice 8): a page past the
  file's end, or of a dead server, is a fault to the task's own handler
  (`SIGBUS`), not a thread ended by `mm`; pages already mapped stay until
  `mm` unmaps them. Unmapping a file page from every mapper needs, per
  page, who maps it: `mm` keeps per-task maps only, so this is a scan over
  the tasks (bounded by `MM_MAX_TASKS`), or a reverse map, which is a new
  record per page.
- **Passive `mm` can't wait on a file server.** `mm` has one thread, no
  context of its own, and serves on its clients' lent time (0023). If it
  calls a file server in a fault, the faulting client's time is lent on
  down the chain (fair: the client pays), but `mm` is blocked until the
  server answers, and so is every other client of `mm`. A slow or hostile
  file server would stop all memory management. So `mm` must not make
  that call itself, synchronously. Options, for the owner:
  1. `mm` saves the faulting thread's reply (`reply_save`) and hands the
     request to a helper thread of `mm`'s (with its own context, or on
     time lent by the client through a second endpoint) that talks to the
     server and finishes the fault; `mm`'s main loop goes on.
  2. The faulting task's own pager runs outside `mm`: the file server maps
     through a region capability (choice 7) that `mm` grants it over the
     faulting task's range. `mm` keeps the frames and can kill the region.
  3. One `mm` thread per file server, each passive, reached through its own
     endpoint.
  The first fits 0023 best and needs nothing new from the kernel.
- **Records:** a region gains its backing (anonymous, or a file: the
  server's endpoint, the file's handle, the offset), and `mm` a page cache.
  The refinement test's abstract machine gains files (a page's value is the
  file's until written).

## Accepted (the owner, 2026-10-29)

**1. Guard pages: `MM_NONE`, as proposed, except for what protecting to
it does to pages already there.** The draft freed them. Then
`mprotect(PROT_NONE)` followed by `mprotect(PROT_READ | PROT_WRITE)` would
have lost the data, where POSIX keeps it, and JITs, garbage collectors and
sanitizers rely on that.

**What is built instead:**
- **Protecting to `MM_NONE`** removes only the kernel's mappings (x86 has
  no present entry that refuses reads). `mm` keeps the pages, their
  references and its records.
- **The region made accessible again:** each page is mapped back at its
  first fault, holding what it held. The kernel reports such a fault as
  *not present* while `mm`'s records have the page, which `mm` used to
  take as "another thread's fault got there first".
  - **`mm` now maps the page.** The kernel's `E_OCCUPIED` means another
    thread did get there first, and the thread is resumed.
  - **A COW page** comes back read-only, and copies at its first write.
- **`cow_break` and a page the kernel doesn't map:** a `map_replace`
  refused with `E_STATE`, the old page not mapped, falls back to a plain
  `map` of the copy (SHARE of such a page).
- **CLONE** keeps a no-access region's pages in the child, referenced
  but not mapped.
- **SHARE** refuses a no-access region.
- **`mm_kops`'s `map` and `map_replace`** return the kernel's error instead
  of 1, so `mm` can tell these two apart (`MM_KE_OCCUPIED`,
  `MM_KE_STATE`).

**2. File-backed regions: option 1.** A request that needs the file
server is handed to a helper thread of `mm`'s (the reply saved with
`reply_save`), which finishes the fault while `mm`'s main loop goes on. A
slow or hostile file server then stalls only the faults on its own files.
Its own scheduling context, or time lent through a second endpoint, is
for Phase 6 to settle with the rest of file-backed regions.

## Consequences

- `MM_NONE` is a small change to `mmlib` (a permission, the fault rule, the
  protect rule) with its tests; nothing in the kernel.
- File-backed regions are Phase 6's (0007): this records what they will
  change, so that `mm`'s records and the refinement test aren't shaped
  against them now. The passive-`mm` problem above is the one design
  question that needs an answer before they are built.

## Priority check

- **Reliability:** a guard page that can't silently disappear; a file
  server that can't stall `mm`.
- **POSIX:** `PROT_NONE`, guard pages as glibc makes them, `MAP_PRIVATE`
  and `MAP_SHARED` file mappings, `SIGBUS` past the end of a file.
- **Reuse:** glibc's `allocate_stack` unchanged.
