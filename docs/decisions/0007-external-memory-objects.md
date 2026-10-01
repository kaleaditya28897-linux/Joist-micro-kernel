# 0007 — External memory objects → page-fault IPC

Date: 2026-09-23
Status: accepted (2026-10-01)
Covers: §6 row *External memory objects (`memory_object_*`)*.
Blocks: Phase 6 (paging protocol and libpager).

## Context

Mach's external pager protocol lets a userspace server back a memory object:
the kernel asks it for pages (`memory_object_data_request`), it supplies them
(`memory_object_data_supply`), and a long-lived bidirectional relationship
exists between kernel, pager and each mapping. §6 maps this onto page-fault
IPC to a userspace pager, and §4.1 marks libpager as a **rewrite**, not an
adaptation.

**Evidence, and its limits.** `docs/phase0-data/mach-call-counts.csv` shows
**zero** `memory_object_*` calls across all three workloads. This is a
measurement artefact, not a finding: `rpctrace` interposes on a task's ports,
and the pager relationship is between the kernel and the translator, which was
not itself traced. `docs/mach-inventory.md` *Next steps* already records that
tracing a translator directly is required. **Do not read the zero as evidence
that the Hurd does not use external pagers — it is how ext2fs serves every
file.**

One call did fire: `device_pager_init_pager` (1, at boot).

This decision is therefore made on architecture, not on counts, and should be
revisited once a translator trace exists.

## Options considered

**A — Port the Mach external-pager protocol.** Keeps libpager mostly intact.
Rejected in §4.1 already: the protocol is large, asynchronous, and full of
lifetime rules between three parties (kernel, pager, mapper) that are exactly
the sort of thing that cannot be bounded cheaply.

**B — Page-fault-as-IPC: on a fault the kernel sends a synchronous message to
the faulting task's registered pager, which replies with a frame capability.**
The relationship becomes request/reply with no kernel-held pager state beyond
one capability per region.

**C — B, but the memory manager (`servers/mm`) is the only pager the kernel
knows**, and filesystem servers talk to the memory manager rather than to the
kernel.

## Decision

**C.**

- The kernel knows one thing: each mapped region has a **pager capability**.
  On a fault it performs a synchronous `Call` to that capability with the
  faulting address and access type, and expects a reply carrying a frame
  capability to install.
- The pager the kernel talks to is normally **`servers/mm`**, the userspace
  memory manager. Filesystem servers do not implement a kernel-facing pager
  protocol; they answer ordinary `io_read`/`io_write`-shaped requests from
  `mm`. This keeps the kernel-facing protocol to one shape with one client.
- **Fault IPC is synchronous and the faulting thread is blocked**, which is
  what makes it accountable: the fault is charged to the faulting thread's
  scheduling context, and priority inheritance applies to the pager (0001,
  §5.3). Mach's asynchronous pager gives no such path.
- **A dead pager is a fault, not a hang.** If the pager capability is dead
  (§5.4), the faulting access raises a fault to the task's fault handler,
  which becomes `SIGBUS`. This is the hook `docs/client-recovery.md` §3 needs
  for the `mmap`-across-filesystem-restart case, and it is why that case has to
  be settled here in Phase 6 rather than in Phase 9.
- No kernel-side page cache, no kernel-initiated pageout. Eviction is `mm`
  telling the kernel to unmap, not the kernel telling a pager to write back.

## Consequences

- Easier: the kernel's paging interface is one call and one reply; libpager's
  rewrite has a small target; `mm` owns all paging policy, per §5.7.
- Harder: every fault costs an IPC round trip to userspace, so the fault path's
  latency is the memory manager's latency. This must be benchmarked from
  Phase 4, alongside IPC itself, or Phase 6 will discover it too late.
- `mm` becomes a critical-path server. It is class C in §7.2 and its restart
  story is genuinely hard, because it holds the mapping state of every task.
- Open: whether a single fault reply can carry more than one frame
  (read-ahead). Almost certainly needed for performance; deliberately left out
  of the first protocol so the simple version is measured first.

## Priority check

Respects the order. Option A is the most Reuse-friendly and loses to
Reliability, as §4.1 already decided when it marked libpager a rewrite. The
read-ahead omission is Performance losing to simplicity, to be revisited with
measurements rather than assumptions.
