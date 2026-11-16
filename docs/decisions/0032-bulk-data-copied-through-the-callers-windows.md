# 0032 — Bulk data crosses by a kernel copy through the caller's windows, not by moving pages

Date: 2026-11-14
Status: **accepted** (the owner, 2026-11-14, as written; its direction,
a copy into a window and not a move, the owner's the same day)
Amends: 0006 (its third mechanism, move-grant: replaced by this copy; a
true move stays possible later, if `io_read`'s measurements ask for it
before Phase 11, as 0006 itself requires). Carries out 0006's second
mechanism (a copy into a receiver's buffer) for both directions.
Covers: 0001 (the call and its reply capability; deferred replies by
`reply_save`), 0030 (regions: pages never COW-shared), `docs/phase5-plan.md`
choice 4 (move-grant through a receive window) and step 9.
Blocks: Phase 5 step 10 (`io_read` from `hello` larger than the IPC
buffer); Phase 11's `io_read` measurement.

## Context

0006 chose three transfers for Mach's out-of-line (OOL) data: in line;
copied into a receiver's buffer; and, for the 54 parameters with
`dealloc` (`io_read`'s data above all), **move-grant**: the pages leave
the sender's map and enter the receiver's. The Phase 5 plan's choice 4
made it a move between regions (0030), since only region pages are never
COW-shared, and left the decision until regions were built.

They are built, and a move between regions doesn't fit them:

- **I12 (0030):** every page mapped in a region is a page of one of that
  region's own extents (windows into frames). A page moved from the
  sender's region into the receiver's is a page of the sender's extent:
  the receiver's region would need to come to hold it, and nothing records
  who holds a page short of per-page records the kernel would have to
  allocate (a frame keeps one map count for all its pages).
- **Pages flow one way:** every page a server moves out is gone from its
  region; it refills from `mm` by RPC, about once an `io_read`, which is
  the cost the move was to save. A client that frees what it got can't
  give it back to the server.
- **TLB:** a move unmaps from the sender, and on several CPUs that is a
  shootdown for each move.

## Options considered

**A. A move with per-page holders** (0006 as written). Each extent capped
at one page table, with a 512-bit holder map; a frame shared as an extent
by both regions, each page held by at most one. Truest to Mach; the most
kernel and model work; pages still flow one way.

**B. A move of whole extents.** The unit moved is an extent (a frame):
I12 holds unchanged, but a server needs many small frames from `mm` and
more extents a region than 8, and still refills per transfer.

**C. A copy by the kernel through windows the caller gives** (the owner's
choice). The caller says which of its memory the server may read and
which it may write; the server, holding the call's reply, copies from or
into it, a bounded amount a call, then replies. Nothing changes hands;
the server keeps its buffer.

**D. Measure first.** Prototype both before choosing: set aside, the
owner choosing C, with the measurement this decision asks for kept.

## Decision

**C, with these calls and rules.**

- **Two windows a thread** (in its thread record): a *read window*, which
  a server serving its call may read, and a *write window*, which it may
  write; each a byte range of the caller's address space, or none.
  `thread_window(kind, va, len)` sets the calling thread's own (only its
  own); `len` 0 clears it. The MIG stub sets them before a call with OOL
  data past the IPC buffer, and clears them after.
- **The write window lies in one of the caller's regions**, and every
  page written must be present and writable there when the copy is made:
  region pages are never COW-shared (0030), so the kernel writes them
  through its direct map without touching anyone else's copy. A page not
  present or not writable stops the copy there (the count says how far it
  got). The read window may be any of the caller's memory: a read changes
  nothing, so a COW-shared page is read as it is; a page not present stops
  the copy (the stub has touched its data first).
- **Two calls for the server, through the reply it holds** (its own reply
  capability, or a reply object holding a saved one: 0001's `reply_save`):

  | Call | What |
  |---|---|
  | `reply_read(robj, off, dst_va, len)` → n | copies `len` bytes at `off` in the caller's read window to `dst_va` in the server's own memory (present, writable) |
  | `reply_write(robj, off, src_va, len)` → n | copies `len` bytes from `src_va` in the server's own memory (present, readable) to `off` in the caller's write window |

  `robj` 0 is the thread's own reply. Each copies at most
  `JOIST_COPY_MAX` bytes (a build-time constant, proposed 64 KiB, set by
  measurement: the copy runs under the kernel's lock, 0016); the server's
  stub loops. Each returns the bytes copied. Refused: no reply held, or the
  caller no longer waiting for it (`E_PEER`); a range outside the window
  (`E_RANGE`); no window (`E_STATE`).
- **Atomicity and lifetime:** each call is one step under the kernel's
  lock and checks everything again; between calls the caller's other
  threads may change its mappings, and a later call sees it. The windows
  mean anything only while their thread waits for a reply: a server can
  reach a caller only through a reply it holds, and only while that caller
  waits on it. Nothing is mapped, unmapped or shared, so no TLB is flushed
  and no page changes owner.
- **Mach's `dealloc`:** what it says for the *sender* is kept by the
  stubs: a server's routine that returns data with `dealloc` has it freed
  by its stub after the reply's copies; a client's `in` data with `dealloc`
  is freed by the client's stub after the call. The receiver gets its own
  copy, which it owns and frees as any memory. Mach's COW is not
  reproduced (0006): a copy made during the call is what the receiver
  sees.
- **Sizes:** the client's stub must size the write window before the call.
  Where a routine says how much it asks (`io_read`'s `amount`,
  `dir_readdir`'s `bufsiz`), that; otherwise a stub default, and data past
  it is refused (`E_RANGE`, the stub's error), never truncated silently.
  The `dealloc` review (step 9) records each of the 54 parameters' bound.
- **Model first:** the windows and both calls in the kernel model, with
  property tests (bytes land only inside the window, only in region pages
  present and writable, only while the caller waits on that reply; a
  refused call changes nothing), CBMC on both calls, and mutations (a
  write outside the window, into a page not in a region, after the caller
  stopped waiting, past `JOIST_COPY_MAX`).

## Consequences

- **Easier:** no change to I12 or to regions; no TLB shootdowns; no page
  changes owner, so nothing flows one way and no `mm` refill; a server
  reuses its buffer. The same two calls serve both directions, and
  deferred replies (a saved reply object) for free.
- **Harder:** a copy costs per byte where a move costs per page: `io_read`
  at large sizes pays a copy the move wouldn't. Measured in step 9
  (`io_read` by copy, 4 KiB to 1 MiB, against the IPC buffer's copy and
  against an estimate of a move), and before Phase 11 (0006's condition). A
  bounded copy under the kernel's one lock is a latency every other CPU
  may wait behind: `JOIST_COPY_MAX` is set by measuring it.
- **The stubs** set and clear two windows, size the write window, and loop
  the copies: joistmig's work, with host tests.
- **Unresolved:** a routine whose OOL `out` size neither side can bound
  ahead (to be found by the review); the stub default and what happens
  past it.

- **The review (step 9, 2026-11-14,** `docs/phase5-data/step9-dealloc/`):
  56 `dealloc` parameters, not 54 (Phase 0's script missed a type defined
  in its parameter); 48 go server to client, 8 client to server. No sender
  uses its memory after sending, so the copy during the call keeps every
  one right. Of the 48 `out`, 24 have no bound known before the call (5 of
  them port arrays); none is reached in Phase 5, so the open question above
  is Phases 6-8's. The stubs must free a `dealloc` sender's memory even
  when the copy fails, and honour `dealloc[]`.

- **The owner's choices on the stubs (2026-11-16),** after the P53's
  measurement and stub design (`docs/phase5-data/step9-copy/`): an `out`
  array with no bound known before the call gets a default write window,
  `JOIST_MIG_OUT_DEFAULT` (64 KiB, one copy call), and a result past it is
  refused, never cut short (settling the open question above); a call
  carries at most `JOIST_MIG_WINDOW_MAX` = 8 MiB through each window (a
  larger `io_read` is a short read, which glibc's `read` loops over);
  `JOIST_COPY_MAX` stays 64 KiB (a 64 KiB copy holds the kernel's lock
  about 2.6 µs on the P53), cold copies to be measured at step 10.

## Priority check

- **Reliability:** every call bounded (`JOIST_COPY_MAX`); no allocation; no
  mapping changed; a write only where the caller said, into pages no one
  else shares.
- **Resilience:** a caller that dies mid-call ends the copies (`E_PEER`), as
  its reply; a server that dies leaves the caller's memory as it was plus
  what was copied.
- **POSIX:** none affected.
- **Reuse:** `.defs` unchanged; the stubs carry it.
- **Performance:** a copy instead of a move for large `io_read`s: measured
  before it's relied on; a move stays possible later, by its own decision.
