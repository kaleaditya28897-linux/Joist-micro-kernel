# 0014 — Mach features the Hurd uses that §6 does not list: futexes, yield, queue limits

Date: 2026-10-01
Status: proposed
Covers: no §6 row. These are Mach features the Phase 0 inventory found in use
that §6's mapping table never listed. Thread-local storage, the fourth such
feature, is covered by 0010; fork's capability-namespace copy by 0011.

## Context

Evidence: the full-build trace (`docs/phase0-data/amd64-full/`, traced through
compile and link after the rpctrace fix, `phase0-data/README.md` stage 14), the
static call sites (`static-call-sites.csv`) and the trap inventory
(`traps.csv`).

**Futexes: `gsync_wait` / `gsync_wake` / `gsync_requeue`.** 698 waits and
1 396 wakes per build, 29 / 58 in the shell workload. Used only by glibc
(`hurd/`, `mach/`, `htl` condition variables): its low-level locks, pthread
mutexes and condition variables all park on them. `gsync_requeue` has one
static user (glibc `hurd/`) and did not fire.

**Yield: `swtch`, `swtch_pri`, `thread_switch`.** These are traps, invisible to
rpctrace, so there are **no counts**. Static users: glibc's spin-lock back-off
and `sched_yield` (`sysdeps/mach/sched_yield.c`, `mach/`), libports and
mach-defpager (`thread_switch`), console-client, libddekit and pfinet
(`swtch_pri`). `syscall_thread_depress_abort` has no user.

**Queue limits: `mach_port_set_qlimit`.** 14 534 per build, by volume one of
the busier port calls. The call sites show two opposite uses:

- **limit 1, for coalescing.** `glibc htl/pt-thread-alloc.c:58` sets it on
  every new thread's wakeup port, which accounts for the volume (thread
  creation is 10 534 per build). Also `hurd/hurdfault.c:203` (fault
  forwarding) and `libcons/vcons-open.c:138` (a notify port). "At most one
  message pending" means "a wakeup is pending or it isn't".
- **limit MAX, for buffering.** The network packet receive ports in
  `pfinet/ethernet.c:213`, `lwip/port/netif/hurdethif.c:180` and
  `eth-multiplexer/ethernet.c:141`. A deep queue absorbs bursts of packets
  sent asynchronously by the device driver.

## Options and decision

### Futexes

**A. A kernel futex keyed by user address**, Linux-shaped: wait on
`(address space, vaddr, expected value)`, wake N. It's the most direct
mapping, but the kernel then needs a waiter table keyed by address. That
collides with 0003: no runtime allocation (waiters must live in the TCB, which
is fine), but wake cost is linear in the waiters sharing a hash bucket,
which is a WCET bound on user-controlled data.

**B. Futexes in userspace on 0001's notification objects**, seL4-shaped.
glibc keeps a table from lock address to a notification capability, created
lazily the first time a lock is contended. `gsync_wait` becomes "re-check the
value, then `Wait` on the lock's notification", and `gsync_wake` becomes
`Signal`. Notification bits are sticky, so a wake that lands between the
re-check and the `Wait` isn't lost: the classic futex race is closed by the
primitive itself. `requeue` is a userspace operation on the table.

**C. Futexes as RPCs to the memory manager.** Every contended lock becomes an
IPC round trip to a server. Rejected: it puts the lock slow path behind a
second scheduler decision, for no gain over B.

**Decision: B.** The kernel gains nothing. The cost is one notification
object per *contended* lock, allocated by userspace (0003), and a lookup table
in glibc's Hurd sysdeps. At 698 waits per build this isn't a fast path, and
correctness and bounded kernel work matter more than raw speed. The wake-N /
wake-all distinction maps onto how many waiters a userspace queue per lock
releases. That queue and its locking are the code to get right, and they are
in glibc, where they can be tested in the Phase 1 host model.

### Yield

**Decision: one `Yield` kernel call** (give up the rest of the time slice).
That covers `swtch` and `sched_yield`. It's trivially bounded and needs no new object.
**Directed yield** (`thread_switch` to a named thread, and `swtch_pri`'s
priority depression) is **deferred**. libports' `thread_switch` use is a
hand-off hint, and priority depression interacts with 0003's scheduler and
0001's priority donation. Add it only with a measured need. Most spin-wait
back-off should go away once locks park on futexes (above).

**Counted (2026-10-01, `docs/phase0-data/README.md` stage 15),** with an
instrumented libc installed on the testbed and then removed. Across the three
workloads, **3 438 processes, every one of which made exactly one
`swtch_pri`, and no `swtch` or `thread_switch` at all**, including 322 `gcc` and
313 `cc1` runs and a 4-thread lock-contention test. The single call is a
start-up handshake in `_hurdsig_init` (`hurd/hurdsig.c:1578`): the new
process spins with `swtch_pri (0)` until its signal thread publishes its
thread port. glibc's own comment calls it unnecessary.

This settles the yield decision. The plain `Yield` call above is enough;
directed yield stays deferred, with nothing in these workloads calling for
it; and the one real use disappears if Joist's signal thread hands its
identity back synchronously at creation (0004/0010), which is the fix the
glibc comment asks for. Not measured: libports' `thread_switch` inside
long-running servers that were started before the instrumented libc was
installed. Check it when servers are ported, not before Phase 3.

### Queue limits

**Decision: no kernel counterpart.** Under synchronous IPC there is no
message queue to limit. The two uses map separately:

- *limit 1 (coalescing wakeups)* → **notification objects**, whose bits
  coalesce by construction. glibc's per-thread wakeup port becomes a per-thread
  notification. That is also the mechanism the futex decision above uses.
- *limit MAX (packet buffering)* → **a shared-memory ring between the driver
  and the network stack**, sized explicitly. That's part of 0012's device
  interface (drivers are userspace and own their buffers) and 0006's bulk
  transfer, not of IPC. It needs re-checking when lwip/pfinet are ported.

The 14 534 calls disappear rather than being mapped.

## Consequences

- Easier: none of the three adds a kernel object or an unbounded kernel
  operation. Futexes and coalescing wakeups both reduce to notifications,
  which 0001 already requires.
- Harder: glibc's Hurd sysdeps grow a futex layer (address → notification
  table, per-lock waiter queue) and lose the per-thread wakeup port. Both
  touch glibc's lowest-level locking, and Phase 7 should build them first and
  test them hardest.
- The network stack's packet path is a design item for 0012, not a mechanical
  port: today it leans on Mach's ability to queue asynchronous messages deeply.

## Priority check

Respects the order. Option A for futexes would be simpler to port (glibc's
`gsync` calls map one-to-one) and is rejected for a kernel WCET bound on
user-controlled data: Reliability over Reuse. Yield stays minimal until
measured: Simplicity over speculative Performance.
