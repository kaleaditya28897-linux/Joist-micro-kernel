# Client-side recovery

Phase 0 deliverable. Expands §7.3 of the plan into a decided answer for every
case, so that the kernel's lifetime notifications (§5.4) and the memory manager
(§5.5) — both built long before the resilience phase — are designed against
real requirements rather than a sketch.

**Status:** first cut. Every case below has a *provisional* decision. Cases
marked **(confirm)** need checking against real Hurd/glibc source on the
testbed before this document is considered closed.

## 1. Why this is the hard part

A restarted server is worthless if its clients cannot resume. RS and DS (§7.1)
are comparatively simple: start a thing, watch a thing, hand back a blob. The
difficulty is in glibc, where every file descriptor is a live capability to a
server that may vanish, and POSIX semantics were never written with that in
mind.

The guiding rule: **never fake transparency.** An application that cannot tell
the difference between "the server restarted cleanly" and "your data is gone"
will silently corrupt something. Where recovery is impossible, the correct
behaviour is a normal POSIX error, not a plausible-looking success.

## 2. The recovery record

One record per **file description** (not per descriptor — see §4):

| Field | Purpose |
|---|---|
| Reconnect key | How to find the object again (§3). |
| Open flags | `O_RDWR`, `O_APPEND`, `O_NONBLOCK`, `O_CLOEXEC`, … |
| File offset | Owned by the description, shared by all its descriptors. |
| Auth state | The identity the object was opened under, so reconnect re-authenticates as *that* principal, not as whoever happens to be running at reconnect time. |
| Class | `recoverable` / `best-effort` / `unrecoverable` (§5). |

Client-side state only. It must not depend on the server having survived.

## 3. Reconnect keys, case by case

| Case | Decision |
|---|---|
| Ordinary file, still linked | **recoverable.** Re-lookup by path, then validate identity (§3.1). |
| Unlinked-but-open (`unlink` after `open`, `O_TMPFILE`) | **unrecoverable** for the first implementation. The alternative — the server republishing an identity token to DS and holding the unlinked object across restart — is a real option but costs filesystem-server work that Phase 9 cannot absorb. Revisit once class B restart works for the simple case. **(confirm)** how much Hurd code depends on this surviving. |
| Path re-bound during the outage (renamed, replaced, translator re-settled) | **fail, do not silently substitute.** Identity validation (§3.1) catches it; return `ESTALE`. |
| Pipes, `pflocal` sockets | **best-effort at most.** Buffer contents live only in the server; if `pflocal` restarts they are gone. Report `EIO` rather than a short read that looks like EOF. |
| Network sockets (lwip/pfinet) | **unrecoverable.** A TCP connection does not survive its stack restarting. Deliver `ECONNRESET`. |
| `mmap` regions | **deferred to Phase 4, not Phase 9.** A file-backed mapping is a standing relationship between the memory manager and the filesystem server, not a request/reply, so the page-fault protocol has to state what happens when the pager dies. Provisional: the memory manager either holds the region's pages or faults the region to `SIGBUS`. This must be settled while designing the paging protocol, because retrofitting it is not possible. |
| cwd and root directory ports | **recoverable**, same as ordinary files. Easy to forget: these are ports too and they go stale identically. |
| Directory streams (`opendir`/`readdir`) | **best-effort.** POSIX already allows unspecified behaviour for entries added or removed during a scan; a restart mid-scan returns `EIO` rather than silently restarting the stream. **(confirm)** against `dir_readdir` usage. |

### 3.1 Identity validation

Re-lookup by path is never sufficient on its own. The reconnect compares an
**inode number plus generation counter** against what was recorded at open
time. Path alone would happily hand the application a different file that
merely has the same name. Requires filesystem servers to expose a stable
generation counter — **(confirm)** whether ext2fs already does.

## 4. Descriptors, descriptions, and fork

The record hangs off the **file description** because `dup`, `dup2`, and `fork`
create several descriptors sharing one description and one offset.
Consequences:

1. Reconnect happens **once per description**, not once per descriptor, or the
   offset is restored several times.
2. Two processes sharing a description after `fork` both observe the death.
   Reconnect is serialised per description; the loser of the race adopts the
   winner's result rather than reconnecting again.
3. A description shared across `fork` cannot be re-opened by a child that has
   since `exec`'d away the path it was opened from — another source of
   **unrecoverable**.

## 5. Idempotency classification

Every reused RPC is classified, and the table is **checked in**, not inferred:

- **Idempotent** — `io_read` at an explicit offset, `io_stat`, `dir_lookup`.
  Safe to replay after reconnect.
- **Non-idempotent** — `io_write` in append mode, directory mutations, anything
  allocating. Replay risks double execution.
- **Unclassified is a build error.** A new RPC without a classification fails
  the build, exactly as a server without a class (§7.2) does not ship.

For a non-idempotent call that was sent but whose reply was lost, the client
**cannot know** whether it happened. Fail with `EIO`. Silent double-execution
is worse than a visible error.

The table itself is generated in Phase 0 from the `.defs` inventory and lives
at `docs/rpc-idempotency.md` once `rpctrace` data exists.

## 6. What the application sees

- `recoverable`: transparent. The application sees a delay, nothing else.
- `best-effort` / `unrecoverable`: a normal POSIX error (`EIO`, `ECONNRESET`,
  `ESTALE`).
- Recovery is bounded: subject to the same timeouts as any blocking operation
  (§5.3), with a bounded number of reconnect attempts. It must not deadlock.
- Signals: a reconnect in progress is interruptible. `SA_RESTART` applies to
  the **original** call, not to the reconnect.

## 7. Where the code lives

One module in the Hurd sysdeps, owning the recovery record, the reconnect state
machine, and the classification table. Not scattered through glibc.

Per the roadmap, the **skeleton lands in Phase 7** with the glibc port, even
though nothing restarts until Phase 9. Retrofitting per-description recovery
records into a finished glibc port is far more expensive than building them in.

## 8. Open questions for the testbed

Answered 2026-09-23 on the testbed (Debian GNU/Hurd i386, glibc 2.43,
hurd 0.9.git20260919 sources). Evidence is in `docs/mach-inventory.md` and
`docs/rpc-idempotency.md`.

1. **Does ext2fs expose a usable inode generation counter?** **Yes, already,
   through plain `io_stat` (`st_gen`).** Test: create, delete and recreate a
   file three times. ext2fs reused inode 227629 each time and returned a
   different `st_gen` each time (1790172149, 1790172413, 1790172515). §3.1
   needs no new RPC for ext2fs; `io_stat` or `io_identity` can carry it. Other
   filesystem servers (tmpfs, isofs, nfs) are unchecked.
2. **How much Hurd code depends on unlinked-but-open files?** Partly answered.
   By construction, glibc's `tmpfile()` (`sysdeps/mach/hurd/tmpfile.c`) and
   `O_TMPFILE` (`hurd/lookup-at.c`) create files with `dir_mkfile`, which are
   *born* unlinked and stay that way. So every `tmpfile()` user depends on it.
   `symlinkat`/`mknodat` also use `dir_mkfile`, but link the node before
   returning, so they are unaffected. That accounts for all 11 `dir_mkfile`
   calls in the build trace (11 `file_set_translator`, 11 `dir_link`). The
   classic `open`-then-`unlink` pattern (for example bash here-documents) is
   **not yet measured**: 2 417 `dir_unlink` calls fired, but nothing yet
   correlates them with ports still open at the time. That needs a pass over
   the trace that tracks port lifetimes.
3. **RPC frequency.** Done. See `docs/rpc-idempotency.md`. The traffic is
   dominated by `dir_lookup` (293k in the build), `io_stat` (211k), `io_read`
   (66k) and `io_seek` (57k).
4. **What is in a Hurd file descriptor?** Almost nothing. The descriptor
   (`struct hurd_fd`, glibc `hurd/hurd/fd.h`) is `{io port, fcntl flags,
   ctty port}`. The flags are per descriptor, i.e. `FD_CLOEXEC`. The
   **description is entirely server-side**: libdiskfs `struct peropen` holds
   `filepointer`, `openstat` (open modes including `O_APPEND`), `lock_status`
   (record locks) and `path`. `dup` takes another user reference on the same
   send right (`dup3.c`, `mach_port_mod_refs`). `fork` recreates the same port
   names in the child (`_Fork.c`; see the `mach_port_names` /
   `mach_port_allocate_name` traffic in the inventory). So dup'd and forked
   descriptors share one `peropen` because they share one port.

   **Consequence for §4, which needs revising:** the client never holds the
   description state that the recovery record is supposed to restore. The
   offset, open modes and locks all die with the server. Within one process,
   the port name identifies the description, so reconnecting once per
   description (§4.1) is keyable. But restoring the offset needs either
   client-side offsets or a protocol change; `docs/rpc-idempotency.md` lays out
   both options. Record locks held at the time of a restart are a new
   **unrecoverable** case missing from §3's table.
