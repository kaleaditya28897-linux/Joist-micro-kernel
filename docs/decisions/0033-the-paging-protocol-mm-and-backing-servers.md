# 0033 — The paging protocol: `mm` and the servers that back files

Date: 2026-11-21
Status: **accepted** (the owner, 2026-11-21, as written, with `cap_read`
returning an endpoint's identity). Phase 6 step 4, `docs/phase6-plan.md`
choices 2-6.
Carries out: 0007 (option C: `mm` the only pager the kernel knows; file
servers answer `mm`), 0024 §2 (file-backed regions; helper threads).
Covers: 0027 (passes with signal and code: the owner's answer of
2026-11-21), 0028 (accounting), 0029 (`mm`'s restart records), 0001
(`reply_save`, call 72 `ipc_buffer`), I4 (dead names).
Blocks: Phase 6 steps 5-8; Phase 8's ext2fs (through libpager).

## Context

Mach backs a file mapping by a *memory object*: the kernel asks the
object's pager for pages (`memory_object_data_request`), the pager
supplies them, and kernel, pager and every mapper keep a long-lived,
asynchronous relationship (precious pages, lock requests, termination).
0007 replaced it: the kernel sends a page fault to one pager, `mm`, as a
synchronous Call; file servers never face the kernel. 0024 settled that
`mm` must not wait on a file server itself (a helper does it) and listed
what file pages change in `mm`. The plan's choices 2-6 gave the protocol's
shape, the helpers (4, at most one busy a server, no timeout), a dead
server's cached pages kept, and eviction by a scan.

What is left is the protocol itself: its messages in both directions,
how `mm` knows two capabilities name the same file or the same server,
what it records, and its bounds.

## Options considered

**A. A file server pages for its own mappings** through a region
capability `mm` grants it over the client's range (0024's option 2). The
server maps its own pages into clients. Rejected: a file server would hold
mapping authority over its clients, and COW across files and anonymous
memory would have two owners.

**B. `mm` asks file servers with ordinary Hurd RPCs** (`io_read`,
`io_write` on the file). Reuses joistmig; but `io_read` reads through the
file's offset and permissions as a client would, can't say "page past the
end" apart from a short read at a race with truncation, and gives the
server no way to tell `mm` to drop or write back pages (libpager's
`pager_sync`, `pager_flush`, `pager_return`).

**C. A small protocol of its own between `mm` and a *pager capability***
that the file server gives out per file (the Hurd's `io_map` result),
served by libpager. Five requests from `mm`, three from the server.

## Decision

**C.**

### Mapping a file

- A client asks the file's server for its **pager capability** (`io_map`,
  as the Hurd does; its two memory-object ports become one capability,
  with or without write permission). It is a libports port in libpager's
  bucket: one endpoint a server, a badge a file (0005).
- The client asks `mm`: `MM_REQ_MAP_FILE(va, pages, file page, perms |
  MM_SHARED)`, the pager capability in the message. `mm` keeps one
  capability a *file* in its file table and drops the duplicate; a mapping
  records its file and offset. glibc's `mmap` of a file becomes `io_map`
  then `MM_REQ_MAP_FILE` (where it was `io_map` then `vm_map` of the
  memory object).

### `mm`'s requests to a pager (a helper calls; the reply in the IPC buffer)

| Request | Reply |
|---|---|
| `PG_HELLO`, a capability to `mm` in the message (first, for each new file) | `OK` and the file's size; or an error (the mapping is refused) |
| `PG_READ(page)` | `OK` and the page (4 KiB in the IPC buffer); `PG_PAST_END`; or an error |
| `PG_WRITE(page)`, the page in the buffer | `OK`, or an error (the page stays dirty, counted) |
| `PG_UNLOCK(page)` | `OK` (a `MAP_SHARED` page may now be written: libpager's `pager_unlock_page`, where ext2fs allocates blocks), or an error |
| `PG_EVICTED(page)` | none needed (libpager's `pager_notify_evict`; best effort) |

- **One page a request,** as 0007 decided; read-ahead waits for a
  measurement. The helper points its IPC buffer at the destination pool
  page (call 72) before `PG_READ`, so the kernel's copy lands in place;
  for `PG_WRITE` at the page to send.
- **No timeout** (the plan's choice 3); a dead server ends the call
  (`E_PEER` or `E_GONE`).

### A pager's requests to `mm` (libpager's `pager_sync`, `pager_flush`, `pager_return`)

| Request | What `mm` does |
|---|---|
| `MM_REQ_PG_SYNC(file, page, n)` | writes back the dirty pages in the range (by helper), answers when done |
| `MM_REQ_PG_FLUSH(file, page, n)` | unmaps the range's pages from every mapper and drops them from the cache, dirty ones written first (`pager_return`: the same, dirty ones not written) |
| `MM_REQ_PG_SIZE(file, bytes)` | the file's new size: pages past it are flushed; later faults past it pass `SIGBUS`/`BUS_ADRERR` |

- The server proves which file by the badge of the capability it calls
  with: `mm`'s `PG_HELLO` gives each new file a capability to `mm`'s own
  endpoint, badged with the file's table index. A server can act only on
  its own files. (`PG_HELLO` is a helper's call too: `MAP_FILE`'s reply
  waits for it.)
- Work that needs the pager (sync, a flush's write-back) is a helper's;
  `mm`'s main loop saves the request's reply and goes on.

### Helpers (the plan's choice 3)

- 4 helper threads, each with a scheduling context of `mm`'s, budgeted.
  A fault on a file page not in the cache: `mm` saves the fault's reply
  (`reply_save`), queues the work, and goes back to its loop. A helper
  takes the oldest queued work whose server has no helper busy.
- **The queue:** at most one entry a faulting thread (128) plus the
  servers' sync and flush requests (one a file): bounded.
- A task that dies while its fault is queued or in hand: the saved reply
  is answered into nothing (`E_PEER`), and the page, if it came, stays in
  the cache.

### Same file, same server: what `mm` must be able to tell

- `cap_read` gives a capability's type, rights and badge, not which
  endpoint it names. To keep one capability a file (and share its pages)
  and to keep one helper a server, `mm` needs the endpoint's identity.
- **Decided (the owner, 2026-11-21):** `cap_read` also returns, for an endpoint capability, an
  identity for the endpoint (its object index and a generation, in `R10`,
  as it already returns a frame's size there). A small kernel change, model
  first. A dead endpoint's index may come back with a new generation, so
  identities never collide.
- *Alternative:* without it, every capability is its own file and its own
  server: no sharing between two clients' mappings of one file, and a
  server that hands out many files can hold all 4 helpers.

### The page cache

- (file, page) → a pool page, in a bounded table; a page is shared by every
  mapping of it.
- **`MAP_PRIVATE`:** mapped read-only; a write copies (COW as now), and the
  copy is anonymous.
- **`MAP_SHARED`:** mapped read-only; the first write fault is a
  `PG_UNLOCK` (by helper), then the page is dirty and mapped writable.
  `msync` is `MM_REQ_SYNC(va, pages)` from the client.
- **Past the end** (`PG_PAST_END`): a pass, `SIGBUS`/`BUS_ADRERR`.
- **Accounting (0028):** a cached page is charged to the tree that first
  faulted it in, and moved to `mm`'s own count if that tree goes while the
  page is still mapped by another; a private copy is charged to its tree.

### Eviction (the plan's choice 5)

- When the pool's free pages fall below a low mark, `mm` evicts file pages
  not mapped writable by anyone, oldest first: each unmapped from every
  task that maps it (a scan of the tasks' maps, bounded by
  `MM_MAX_TASKS`), dirty ones written back first (by helper), then
  `PG_EVICTED`. Measured; a reverse map replaces the scan if it costs too
  much. Anonymous pages are never evicted.

### A server that dies (the plan's choice 6)

- `mm` asks for a dead-name notification on each file's capability (I4).
  At the death: faults on its pages not cached pass `SIGBUS`/`BUS_OBJERR`;
  cached pages stay mapped and served; dirty pages can't be written back
  and are dropped at unmap or eviction, the loss counted. Phase 9's client
  recovery may re-attach a file to a restarted server.

### Codes (the owner's answer of 2026-11-21)

A pass's code carries glibc's code in bits 0-15 and the signal in bits
16-23. `mm` passes: `SIGSEGV`/`SEGV_MAPERR` (no region),
`SIGSEGV`/`SEGV_ACCERR` (not allowed), `SIGBUS`/`BUS_ADRERR` (past a file's
end; no page to be had: the tree's ceiling or the pool), and
`SIGBUS`/`BUS_OBJERR` (the pager dead, or its error).

### `mm`'s records (0029)

New: the file table (capability slot, endpoint identity, badge, size, the
mm-side capability given to the server, dead mark), the page cache (file,
page, pool page, dirty), the work queue and the saved replies, the
helpers. Each is listed against 0029: the file table and cache are
restartable from `mm`'s state frames; saved replies and the queue are not
(a restart fails those faults, as it fails a request in hand today).

### Bounds (proposed, set by measurement later)

`MM_FILES` 64, the cache up to the pool, the queue 128 + `MM_FILES`, 4
helpers.

## Consequences

- **Easier:** the kernel is unchanged but for `cap_read`'s identity;
  libpager's users keep their callbacks; a slow or dead file server
  stalls only its own files; `mm` keeps every mapping decision.
- **Harder:** `mm` roughly doubles; its restart has more to rebuild; every
  file page costs a copy through the IPC buffer (no zero-copy until
  measured worth it); eviction's scan grows with the tasks.
- **Unresolved, later:** read-ahead (0007), a reverse map (if the scan is
  slow), write-behind policy beyond sync and eviction, swap for anonymous
  memory (a default pager, not Phase 6).

## Verification

- **Host first (`mmlib`):** the refinement test's abstract machine gains
  files (a page's value is the file's until written; a shared write
  reaches the file at sync; a private write never does); a simulated
  pager with fault injection (slow, error, past the end, death); helpers:
  never two busy on one server, a hung server stalling only its own
  files; eviction under a small pool. Mutations: a dirty page dropped
  silently, a private write reaching the file, a page past the end
  served, a cached page charged twice, two helpers on one server, a dead
  server's cached page unmapped.
- **The kernel's `cap_read` identity** in the model, with its tests and a
  mutation (two endpoints given one identity).
- **On Joist:** the synthetic pager's stress test and libpager's test
  translator (the plan's choices 8 and 9).

## Priority check

- **Reliability:** `mm` never waits on a file server; every queue and
  table bounded; one hostile server holds one helper.
- **Resilience:** a dead server is `SIGBUS` on what isn't cached, never a
  hang; what is cached keeps working.
- **POSIX:** `MAP_PRIVATE`, `MAP_SHARED`, `msync`, `SIGBUS` past the end.
- **Reuse:** libpager's interface to ext2fs and storeio unchanged.
- **Performance:** one page a request and a copy a page; measured before
  read-ahead or zero-copy are added.
