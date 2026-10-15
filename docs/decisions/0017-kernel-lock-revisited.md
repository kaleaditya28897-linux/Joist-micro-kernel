# 0017 — The kernel lock revisited: one lock kept, the timer written outside it

Date: 2026-10-15
Status: **accepted** (2026-10-15).
Revisits: 0016 (its "measured, then revisited" clause), at the owner's
request after Phase 3 step 9's measurements.
Covers: §5.1 (bounded WCET), §5.6 (SMP), 0003 (lock order), Phase 3
choice 9 (the O(1) run queues).

## Context

0016 chose one kernel lock, interrupts off and preemption points, and said
the lock's cost would be measured in Phase 3 and the decision revisited if
it mattered. Step 9 measured it under KVM on 4 pinned vCPUs:

- **Uncontended:** about 45 cycles a kernel entry. A round trip between
  address spaces went from 1 356 to 1 444 cycles, the same on 2 and 4 CPUs.
- **Contended, worst case:** with every other CPU doing nothing but system
  calls, that round trip takes about 4 300 cycles on 2 CPUs and 9 300 on 4.

On 2026-10-15 the owner withdrew their acceptance of the scans that stood
in for choice 9's run queues ("Nothing can be allowed to compromise our
goals"). The queues are now built (`docs/phase3-plan.md`, *The run queues*):
choosing a thread costs O(CPUs), not O(threads), for 172 cycles more on an
uncontended round trip.

The record also shows that 0016's text and the code differ in three places.
An entry is never edited, so this one records the differences:

1. **Preemption points.** 0016 says a cascade checks "for a pending
   interrupt" between units. The kernel instead does one *piece* per kernel
   entry, of at most `RUN_PREEMPT_SLOTS` (32) slots (`kernel/core/task.c`,
   `k->preempt`), and then looks at what is due. It never polls the
   interrupt controller. The bound 0016 wanted holds: interrupts are off for
   at most one piece. The mechanism is different.
2. **Per-CPU state without the lock.** 0016 lists the run queues among the
   per-CPU state "reached without the lock where only its CPU touches it".
   Scheduling is global: any CPU may choose a thread from any CPU's queue,
   and any operation may queue a thread on another CPU's queue. So the run
   queues are under the one lock, like everything else in `kernel/core/`.
   Only the kernel stack, the per-CPU record and the timer stay outside it.
3. **Retire slots** (0003, which 0016 cites) were not built. With one lock
   and no lock-free readers, nothing reads an object after it is freed, so
   none are needed.

## Evidence: the lock study (2026-10-15)

Run on the P53 by its Claude Code; its report, analysis, scripts and every
kept serial log are in `docs/phase3-data/step10-lock/` (`findings.md`
first). First-level KVM, QEMU 11.1.2, 4 vCPUs pinned to 4 cores,
i7-9850H; every boot kept only if the build VM stayed under 2 % busy
throughout; 7 to 16 boots per variant; medians over boots. The tree is
`93fb28d` (the scan, before the run queues), plus instrumentation that is
off by default (`RUN_LOCK_STATS`, `RUN_LOCK_MCS`, `RUN_LOCK_TIMER_OUT`) and
`user/scaleroot`: K independent client/server pairs calling for 2 s at the
same moment.

**1. Throughput does not rise with CPUs; it falls.** Total round trips per
second, against one pair alone:

| K pairs | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| Passive server (direct switch) | 1.00 | 0.73 | 0.66 | 0.57 |
| Server with its own context | 1.00 | 0.90 | 0.88 | 0.85 |

With 4 pairs the CPUs spend **75 % of their time waiting for the lock**.

**2. What a hold is made of.** Instrumented (so upper bounds), mean cycles
per acquisition at K = 4:

| | Passive (fast path) | Own context (slow path) |
|---|---|---|
| The IPC operation (`abi_syscall`) | 403 | 425 |
| Address-space switch (CR3) | 330 | 333 |
| **Timer write** (a VM exit) | rare | **6 383** |
| Scheduler scans (pick, next event, `poke_others`) | none | 974 |
| Rest | about 560 | about 780 |
| **Mean hold** | **1 293** | **8 891** |

A passive hold grows from 865 cycles at K = 1 to 1 293 at K = 4 with no
extra work done. That fits the kernel's shared records moving between the
cores' caches on every acquisition. It is an inference: no cache counters
were available.

**3. An MCS queue lock changes nothing.** Scaling is within 1 to 3 % of
the ticket lock's at every K. Uncontended it costs 52 cycles more on a
do-nothing call and 56 to 68 more on a round trip. With at most 4 waiters,
the handover isn't what they wait for: the hold is.

**4. Writing the timer after the unlock** (`RUN_LOCK_TIMER_OUT`: the time
is chosen under the lock as now, and only this CPU's own LAPIC timer is
written after it) takes the own-context hold from 8 891 cycles to 2 439.
It turns 0.85 into **3.04 times** one pair's throughput at K = 4 (1.73 at
K = 2, 2.49 at K = 3). Passive scaling is unchanged, since the fast path
writes no timer. It ran clean in 14 boots, but no argument for its
correctness has been made yet.

**5. Pause-loop exits** are not a measurable cost: at most 100 to 300 a
second per vCPU, against millions of acquisitions.

**Found on the way:** `benchroot` started its passive servers without the
start-up handshake `docs/abi.md` requires on SMP, and once hung on 4 CPUs.
Fixed with the study's patch.

## Options considered

**A — Keep 0016 as built.** Change nothing. It leaves a known 3.5-fold loss
in the own-context case that one CPU-local change removes.

**B — One lock; nothing CPU-local inside it.** Keep the single lock and
every argument 0016 made for it: the model's sequential semantics are what
runs, and one lock means no lock order. Move out of the hold the work that
touches only this CPU:

- the **timer write**, as measured (item 4);
- later, if measured worthwhile, the **CR3 write** (330 cycles). That needs
  more than a measurement, because the TLB shootdown reads other CPUs'
  `cur_space` under the lock.

**B' — B with an MCS lock.** Measured: no gain, a cost uncontended.
Rejected.

**C — Finer-grained locking** (0016's option A): a lock per endpoint or
per object, and per-CPU scheduler locks. This is the only option that
would make passive IPC on different CPUs scale (item 1's 0.57). But every
operation then needs its own locking argument. The model, its property
tests, CBMC and TLA+ assume one operation at a time, and the differential
test of the run queues against the scan depends on that too. It has the
largest correctness cost, for the goal ranked last.

**D — A clustered multikernel** (seL4's answer for large machines): one
kernel instance, with its own lock, per cluster of CPUs that share a cache;
the clusters communicate by IPC. It keeps every instance sequential, but
needs cross-cluster IPC and capability semantics that do not exist yet.
§5.8's target machines are small.

## Decision

**B.**

- **The one kernel lock stays, and so does the ticket lock.** The MCS lock
  was measured and does not help.
- **The timer is written after the unlock.** The moment is chosen under
  the lock, as now; only the write to this CPU's own LAPIC, and
  `c->armed_for`, which only this CPU reads, move out. Before it is the
  default, the argument is written down (in `docs/phase3-plan.md`): an
  interrupt that arrives between the unlock and the write, or a remote
  "choose again" request, ends in a kernel entry that recomputes the
  moment under the lock, so nothing is lost. Every self-test and the
  checking kernels must pass with it.
- **The CR3 write** moves out only after the shootdown's reading of
  `cur_space` has been reworked and the gain measured. That is not part
  of Phase 3.
- **The revisit trigger is a number.** C or D is reopened by a new entry
  if any of these holds:
  - a measured workload on the target machines spends more than 10 % of
    its CPU time waiting for the lock;
  - a machine with more than 8 CPUs becomes a target;
  - the Hurd's servers, from Phase 5 on, are measured to need passive IPC
    on different CPUs to scale (today 4 pairs make 0.57 of one).

  The lock statistics build (`RUN_LOCK_STATS`) and `user/scaleroot` stay
  in the tree so that any phase can check this.

## Consequences

- Easier: everything 0016 made easy stays easy. The verification story is
  unchanged, and the lock order is still one lock long.
- Easier: servers with their own contexts scale with CPUs (3 times at 4 in
  the measurement), which is the common case for the Hurd's servers.
- Harder: passive IPC on several CPUs at once does not scale (0.57 of one
  pair at 4), and nothing in B changes that. Each CPU's kernel work is
  serialised and its data moves between caches. The trigger above is what
  reopens it.
- Harder: one more piece of per-CPU state, the timer, lives outside the
  lock, with its argument to keep true.
- 0016's wording is corrected here, not edited: preemption by a piece per
  kernel entry, global scheduling, run queues under the lock, no retire
  slots.

## Priority check

Reliability > Resilience > POSIX > Reuse > Performance. B keeps the single
lock that keeps the verified semantics true of the running kernel
(Reliability). It buys Performance only with a CPU-local change whose
correctness can be argued and tested. C would buy Performance with
Reliability, which the order forbids. A would leave a measured 3.5-fold
loss in place for no reliability gain. D is premature for §5.8's machines.
