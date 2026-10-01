# 0006 — Out-of-line memory → typed bulk transfer, not one mechanism

Date: 2026-09-23
Status: accepted (2026-10-01)
Covers: §6 row *Out-of-line memory*. This row keeps its **known-hard** status.

## Context

§6 maps Mach out-of-line memory onto "shared memory grant" and notes this is a
semantics change, not a swap: Mach OOL is copy-on-write with sender-side
deallocation, so the sender may hand over a region and have it leave its map,
while the receiver gets a private view whose lifetime is independent.

The inventory says how much of the Hurd actually depends on that.

**Evidence** (`docs/phase0-data/ool-params.csv`, 140 OOL parameters across the
Hurd's `.defs`):

| | Count |
|---|---:|
| OOL parameters total | 140 |
| direction `in` | 82 |
| direction `out` | 58 |
| **carrying a `dealloc` flag** | **54** (49 `dealloc`, 3 `CountInOut Dealloc`, 2 `dealloc[]`) |
| plain copy, no ownership transfer | 86 |

Spread across `fs.defs` (23), `exec.defs` (16), `process.defs` (13),
`msg.defs` (13), `auth.defs` (13), `socket.defs` (10), `fsys.defs` (10).

The hot path is small and precise:

```
io.defs  io_write        in   data   data_t   (no dealloc)
io.defs  io_read         out  data   data_t   dealloc
```

So `io_read` — the single most performance-relevant bulk transfer in the
system — is exactly the ownership-transferring case, and `io_write` is exactly
the case that is not.

## Options considered

**A — One mechanism: shared memory grants everywhere.** Replace every OOL
parameter with a grant. Fails for the 54 `dealloc` parameters, whose callers
assume the buffer becomes theirs; a shared grant makes lifetime a negotiation
neither side currently performs.

**B — One mechanism: always copy.** Correct for all 140, and throws away the
reason OOL exists. `io_read` at bulk sizes would copy twice.

**C — Three mechanisms, chosen per parameter at MIG-generation time**, driven
by the `.defs` annotation that already exists.

## Decision

**C.** The MIG backend emits one of three transfers per OOL parameter, and the
choice is derived from the existing `.defs` flags — no `.defs` edits for the
common case:

1. **Inline copy** for small, bounded parameters. The threshold is a tunable
   with a measured default, not a guess; most of the 86 plain-copy parameters
   (id arrays, name strings, small records) are in this class.
2. **Copy into a receiver-provided buffer** for larger `in` parameters without
   `dealloc` — notably `io_write`. The sender keeps its memory; nothing is
   shared; the kernel copies directly thread-to-thread (0001).
3. **Move-grant** for the 54 `dealloc` parameters — notably `io_read`. The
   pages leave the sender's map and enter the receiver's. This reproduces
   Mach's ownership transfer honestly, rather than pretending a shared mapping
   is the same thing.

**Copy-on-write is not reproduced.** Mach's OOL is COW underneath; Joist's
move-grant is not. Where a sender needs to keep its copy *and* hand one over,
it copies explicitly. The Phase 5 work is to find those senders — the
measurement above says there are at most 54 candidate sites and probably far
fewer that genuinely rely on retaining the data.

**The threshold and the grant path must be measured before Phase 11.** `io_read`
is the call that decides whether this design is fast enough.

## Consequences

- Easier: each of the three paths is simple on its own, and the `.defs` already
  carry the annotation that selects between them, so most call sites need no
  source change at all.
- Harder: three paths in the MIG backend instead of one, and a per-parameter
  decision that has to be right. The 54 `dealloc` sites need individual review;
  this is the multi-week Phase 5 cost §6 warns about, and the measurement
  bounds it rather than removing it.
- Unresolved: whether any caller depends on OOL's COW behaviour for
  *correctness* rather than performance — i.e. writes to its copy after
  handing it over and expects the receiver not to see it. Grep for post-send
  writes at the 54 sites during Phase 5. **This is the one thing that could
  still make this row expensive.**

## Priority check

Respects the order. Option B is the most reliable and simplest and loses only
where measurement shows the cost is real and confined. Performance is allowed
to select the mechanism only within a set of choices that are all bounded and
allocation-free, so Reliability is not traded.
