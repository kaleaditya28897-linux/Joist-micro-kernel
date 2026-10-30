# 0031 — Reused Hurd code runs on glibc from Phase 5: the port starts early

Date: 2026-10-30
Status: **accepted** (the owner, 2026-10-30, with `docs/phase5-plan.md`;
the direction, glibc brought forward rather than a runtime of Joist's own,
chosen by the owner the same day)
Covers: §4 (glibc: replace `sysdeps/mach` with `sysdeps/joist`, keep the
Hurd layer), §10 (POSIX through glibc), §11 Phases 5 and 7, 0002 (Clang
for glibc), 0006 (OOL), 0008 and 0030 (`malloc` over a region), 0027
(signals, Phase 7).
Revises: §11's split of work between Phase 5 and Phase 7. Phase 7's exit
stays as written.

## Context

Phase 5's exit asks for unmodified-logic Hurd server code (libports,
libtrivfs and a "hello" translator) running on Joist through retargeted
MIG stubs. That code is written against glibc: its threads (`pthread_*`,
glibc's `htl`), `malloc`, `error`, `argp`, and glibc's Hurd layer
(`hurd/`), which itself calls Mach (`mach_msg`, `mach_port_*`, `vm_*`)
through `sysdeps/mach`. In the plan glibc arrives in Phase 7.

## Options considered

**A. A small runtime of Joist's own** (the plan's first draft): only what
an inventory of the libraries' undefined symbols lists. Smallest Phase 5;
but a second C library to keep until Phase 7, and every Hurd interface
the libraries use (argp, `error`, `hurd/` helpers) rewritten once here
and again in glibc.

**B. Bring the glibc port forward** (the owner's choice, 2026-10-30).
Start `sysdeps/joist` in Phase 5, with exactly what the reused servers
need, and run them on the real libc from the start. Phase 5 grows; Phase 7
shrinks by the same work. Nothing is written twice.

**C. musl as an interim libc.** It lacks glibc's Hurd interfaces, so it
needs A's shim as well, and is a second libc too.

## Decision

**B. Phase 5 starts glibc's `sysdeps/joist`, static only, scoped to what
the reused servers and their test clients call.**

- **In Phase 5:**
  - `sysdeps/joist` for the Mach layer the Hurd layer stands on: message
    send and receive over Joist's IPC through the retargeted MIG (the
    backend is built first, since glibc's own Hurd RPC stubs are
    MIG-generated); `mach_port_*` over capabilities (a port name is a
    slot; port sets are badges, 0005); `vm_*` over `mm` and regions
    (0030; `malloc`'s arenas in a region); tasks and threads over Joist's
    calls.
  - `htl` (glibc's pthreads for the Hurd) over Joist threads, each thread
    with a scheduling context, and notifications for its waits.
  - Startup for a server started before `proc` and `auth` exist, as the
    Hurd's boot filesystem is started today: its ports handed to it at
    start, no `proc` registration. Joist's root plays the boot
    filesystem.
  - Static linking only. Built with Clang (0002, accepted: the Clang
    branch, with its two conditions; condition 1, the first
    Linux-to-Hurd cross-build of glibc with Clang, is checked here).
- **Left in Phase 7, as the plan says:** `fork` and `exec` from glibc,
  signals (0027's exception handler), dynamic linking, `posix_spawn`, the
  client-recovery module, and anything the Phase 5 servers don't call.
  Phase 7's exit is unchanged.
- **A check on the size, pre-committed** (as 0002 did for the compiler).
  Step 1 of the work is an inventory: the glibc functions and Hurd-layer
  paths that libports, libihash, libtrivfs, `trans/hello` and a test
  client reach, by building them statically against glibc with
  `sysdeps/mach` removed and listing what fails to link. The inventory
  sets the Phase 5 glibc scope and an estimate. **If the estimate passes
  6 weeks**, the owner decides, on that measurement, whether to go on, to
  narrow the scope (for example libports and a raw MIG server first,
  libtrivfs later), or to fall back to A.

## Consequences

- **Easier:** reused Hurd code runs on the libc it was written for, so
  "unmodified logic" means just that; nothing in Phase 5 is thrown away;
  Phase 7 begins with the Mach layer, threads and `malloc` already
  working and tested.
- **Harder:** Phase 5 grows (its plan said 2-3 months; with this, more,
  estimated after the inventory); glibc's build, a cross-build for a
  new target, joins Joist's build and tests; 0002's condition 1 is met
  earlier, so a compiler surprise surfaces in Phase 5.
- **The plan:** §11's Phase 5 gains the glibc start, and Phase 7 notes
  what it inherits (edited on acceptance, 2026-10-30).

## Priority check

- **Reliability:** glibc's threads are made bounded on Joist (a context
  each, a fixed number a server); `malloc` over a region is accounted
  (0028, 0030). Nothing here buys reuse at reliability's cost; where
  glibc's Mach layer assumed unbounded kernel behaviour (asynchronous
  messages, kernel-buffered OOL), `sysdeps/joist` does what Joist does.
- **Resilience:** unchanged; the client-recovery module stays Phase 7's,
  as the plan says.
- **POSIX:** earlier: the real libc from Phase 5.
- **Reuse:** the reason for B: the Hurd's code and glibc's Hurd layer are
  used as they are, once.
- **Performance:** no change of its own.
