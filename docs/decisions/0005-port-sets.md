# 0005 — Port sets → badge demultiplexing on one endpoint

Date: 2026-09-23
Status: accepted (2026-10-01)
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

## Amendment, 2026-10-01: the full-build count

The evidence above stopped at the first `make` (rpctrace's `gsync_wait`
deadlock). Traced through compile and link (`docs/phase0-data/README.md`
stage 14), `mach_port_move_member` fires **41 times**, against 522 390
`mach_port_deallocate`. That's six times the figure quoted, and still four orders of
magnitude below the hot calls. The conclusion stands: membership is static
in practice, and tear-down-and-re-mint is fine at this frequency.

## Amendment, 2026-10-01: the one port-set use badges do not cover, `select`

The decision above is about **server** buckets. The inventory also found a
**client** use of port sets, and it was only inferred until today. It is now
confirmed from source. glibc's `select`/`poll` (`hurd/hurdselect.c`) sends an
`io_select` request to the server behind each descriptor, each with its own
reply port. Then, in the code's own words (line 324), "We've got multiple
reply ports, so we need a port set to multiplex them": it allocates a port set
(line 331), moves each reply port into it (line 335), and receives on the set
(line 423).

**Under 0001 that shape does not exist.** A reply capability lives only inside
a blocking `Call`, and asynchronous events are notification bit-sets, never
queued messages. So "N outstanding requests, then wait for whichever reply comes
first" has no direct mapping, and option B above does not address it: it is
not a server demultiplexing clients, it is a client waiting on servers.

Two shapes fit 0001's primitives:

1. **One helper thread per watched descriptor**, each doing a blocking
   `io_select` `Call` with the timeout, and signalling a bit on a notification
   object the caller `Wait`s on. It needs no protocol change. The cost is a
   thread per descriptor per `select` (a per-process pool softens it), and a
   cancellation path for the calls still in flight when the first one fires.
2. **A readiness-notification protocol** (§6's "notification-bound endpoint
   group"). `select` makes one short `Call` to each server to register a
   badged signal capability on its notification object, and the reply carries
   the descriptor's current readiness. It then `Wait`s; a server signals the
   bit when its object becomes ready; `select` deregisters on the way out.
   There are no queued messages and no threads, and it's the shape 0001's
   "async events are bit-sets" rule asks for. The cost is a change to
   `io_select` in every server that implements it: libdiskfs, libtrivfs,
   libnetfs, pfinet, lwip, pflocal, term, console-client, boot and seven
   translators under `trans/` (`grep S_io_select`, hurd 0.9.git20260919). Most
   servers inherit it from the three libraries. It also depends on 0001's open
   question 3 (bit-set width): more watched descriptors than bits means bits
   identify groups, and `select` re-queries the group.

**Recommendation: 2**, with 1 as the fallback if the bit-set width (0001 Q3)
turns out to be very small. Frequency gives no urgency: `io_select_timeout`
fired 7 times in the measured build (`phase0-data/hurd-rpc-counts.csv`). But
`select` is on the critical path of every interactive program and network
server, so this must be decided before Phase 5's libports and MIG work, not
discovered during it. **This row stays "ordinary" for server buckets, and
gains one known-hard sub-case: client-side `select`.**

## Accepted, 2026-10-01

Accepted with the 2026-10-01 `select` amendment's recommendation adopted:
**`select`/`poll` use a readiness-notification protocol.** `select` registers a
badged signal capability with each server, then `Wait`s on one notification
object, and servers signal when ready. The `io_select` change in every server
that implements it is Phase 5 work. The bit-set width it depends on is
0001's open item 3.

## Priority check

Respects the order. The decision trades a theoretical Reuse convenience
(libports needing no change) for a smaller kernel, which serves Reliability.
Measurement is what made the trade safe rather than a guess.
