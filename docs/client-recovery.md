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

1. Does ext2fs expose a usable inode generation counter? (§3.1)
2. How much Hurd code depends on unlinked-but-open files surviving? (§3)
3. What does `rpctrace` show for the actual frequency of each RPC, so the
   idempotency table can be ordered by what matters?
4. What is in a Hurd file descriptor today — what hangs off the descriptor vs.
   the description, and how is that shared across `fork`?
