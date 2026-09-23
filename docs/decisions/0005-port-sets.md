# 0005 — Port sets → badge demultiplexing on one endpoint

Date: 2026-09-23
Status: proposed
Covers: §6 row *Port set*.

## Context

§6 maps Mach port sets onto "multiple badges on one endpoint, or a
notification-bound endpoint group", and §6's follow-up note flags this as one
of two rows that may not be a one-line swap: libports has per-port handlers and
can move a port between sets at runtime, which is not obviously the same shape
as badge demultiplexing on a single endpoint.

The inventory settles it.

**Evidence** (`docs/phase0-data/mach-call-counts.csv`):
`mach_port_move_member` fires **3 times at boot, 0 in the shell workload, and 7
across the entire build** — against `mach_port_deallocate` at 343 547 and
`mach_port_insert_right` at 57 434 in the same trace.

Port set *membership* is essentially static. A server builds its bucket at
startup and then receives on it; it does not shuffle ports between sets as a
matter of course. The runtime-reconfiguration capability that made this row
look risky is, in practice, almost unused.

Caveat: this is a dynamic measurement with known coverage limits
(`docs/mach-inventory.md` *Coverage*) — the build trace stops at `configure`
because rpctrace deadlocks on `gsync_wait`. A server that reorganises its
bucket under load after that point would not show up. The static cross-check
(`static-call-sites.csv`) should be read before treating 7 as the final word.

## Options considered

**A — A first-class port-set kernel object.** Receive rights join and leave a
set; the kernel demultiplexes. Faithful to Mach, and adds a kernel object whose
only justification is a call that fires 7 times per build.

**B — One endpoint per server bucket; each client gets a badged send
capability; the server demultiplexes on the badge.** The badge *is* the
per-port identity, so libports' per-port handler table becomes a userspace
hash from badge to handler — which is what libports already is, above the port
layer.

**C — B, plus a kernel "move badge between endpoints" operation** for the rare
reconfiguration case.

## Decision

**B**, with **C explicitly deferred** rather than rejected.

- A server bucket is **one endpoint**. Every client capability to it carries a
  badge. libports keeps its handler table, keyed by badge instead of by port
  name; this is a change of key, not of structure.
- Because membership is static in practice, the reconfiguration path is
  handled by **tearing down and re-minting** capabilities rather than by a
  kernel move operation. At single-digit frequency, the cost is irrelevant.
- If the Phase 5 libports spike finds a real pattern that needs live movement,
  add the operation then, as an amendment here, with the call site that
  justifies it. Do not add it speculatively.

## Consequences

- Easier: one less kernel object and one less kernel operation, directly
  serving the §5.1 "small enough to understand" goal and the ~15k line
  tripwire. Badge demultiplexing is already needed for everything else, so
  this row costs no new mechanism at all.
- Harder: any libports code that assumes it can move a port between buckets
  cheaply must be found and changed. The static call sites are the place to
  look, and this is Phase 5 work.
- **This row is downgraded from "known-hard" to ordinary.** §6's note should be
  amended: the risk was real before the measurement and is not after it. The
  out-of-line memory row (0006) keeps its known-hard status; this one does not.

## Priority check

Respects the order. The decision trades a theoretical Reuse convenience
(libports needing no change) for a smaller kernel, which serves Reliability.
Measurement is what made the trade safe rather than a guess.
