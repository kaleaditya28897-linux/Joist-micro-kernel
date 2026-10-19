# 0018 — One CPU woken for each thread, in a fixed order

Date: 2026-10-16
Status: **accepted** (2026-10-16). The owner chose it from the options in
the session of that date, after the contended-line study below.
Covers: §5.6 (SMP), 0016 and 0017 (the one lock and its pokes), Phase 3's
exit criterion (*IPC benchmarks stable*).

## Context

When an operation makes a thread ready, the kernel interrupts every other
CPU that would now choose differently (`poke_others`, `run.c`, through
`joist_sched_better`). For one new thread and several idle CPUs, that is
every idle CPU. All of them wake and race for the lock. The first takes the
thread, and the rest find nothing and sleep again. Which CPU wins is decided
by timing.

The exit benchmark's contended line ("two address spaces, passive server,
every other CPU making system calls") still misses the stability criterion
after the change of 2026-10-16 (a call that changes nothing skips the
scheduler): its median spreads 6.4 % over 10 boots against the 5 % allowed
(`docs/phase3-data/step10-stays/`). The P53's Claude Code measured it
(`docs/phase3-data/step10-contended2/`, 370 boots):

- The lock is a full convoy. The client finds all three busy CPUs ahead of
  it in 98.7-99.9 % of its acquisitions, every round, every boot. Nothing
  about the queue changes.
- **About 47 % of the boot-to-boot variance is which virtual CPU the client
  lands on**, which is decided at each boot by the race above. With fewer
  busy CPUs, placement is almost all of it: each (client, busy) pair of
  cores has its own level, repeatable to < 1 %, and the levels span 32 %.
- The rest: a host clock dip 45-65 ms after the busy threads start, which
  moves the worst round and p99 but not the median, and a residual SD of
  1.7 % from boot to boot that could not be attributed.

## Options considered

1. **Wake one CPU per thread, in a fixed order** (this decision).
2. **The same, then record the line as a limit of the testbed** if it
   still misses.
3. **Change the benchmark:** measure the line several times per boot with
   fresh threads (new placements) and take the median, plus a 300 ms lead
   for the busy threads. This changes what the line reports.
4. **Record the miss** and leave both the kernel and the benchmark as they
   are.

## Decision

The kernel decides which CPUs to interrupt, and it decides that in the core
(`joist_sched_pokes`, `kernel/core/sched.c`), where the model tests it:

- Each queued thread that some CPU should now run is claimed by **one**
  CPU. Idle CPUs are asked first, in CPU order. Then busy ones are asked,
  starting with the CPU whose thread would go last (a thread that can't run
  at all counts as last). Each CPU's choice leaves out the threads already
  claimed.
- A CPU already interrupted, whose interrupt hasn't yet arrived, is not
  interrupted again. Its choice is counted as a claim.
- A CPU whose own thread can no longer run is interrupted as before.
- The kernel runs this after every choice made while the resched flag is set
  or any interrupt is still on its way, since a CPU may have taken a
  thread another had claimed.

So a thread made ready when several CPUs are idle wakes the lowest-numbered
idle CPU and no other. Placement then follows the order of events, not the
race. The race decided only placement, never correctness: whatever order
the interrupted CPUs choose in, every thread that should run gets a CPU
(the property `test_smp.c` checks, with the interrupts delivered in random
orders).

## Consequences

- Fewer interrupts: one per thread made ready, not one per idle CPU, and
  no CPU wakes to find nothing.
- An idle CPU is preferred to preempting a busy one, and the busy CPU
  preempted is the one running the thread that goes last. Before, every CPU
  that would choose otherwise was interrupted and the race chose, which could
  preempt one thread only to move another.
- Cost: under the lock, O(CPUs × (CPUs + priority words)) per pass,
  against O(CPUs × priority words) for the loop it replaces. No scan of the
  threads.
- Whether the contended line then meets the criterion is measured, not
  assumed. The residual 1.7 % per boot leaves it uncertain. The result
  goes in `docs/phase3-plan.md` and the exit record, whichever way it
  comes out.

## Priority check

Correctness first: the property that no thread is left waiting while a CPU
could run it is held for every delivery order. Then bounded work under the
lock: unchanged in kind, O(CPUs²). Then the exit criterion, which this
serves.

## Outcome (2026-10-19)

Built, verified and measured (`docs/phase3-plan.md`, *Decision 0018*;
`docs/phase3-data/step10-pokes/`, `step10-place0018/`). Kept by the owner's
choice.

- **It does what it says for idle CPUs:** the three busy threads go to the
  lowest idle CPUs, in order, in every boot.
- **It did not fix the client's placement.** The root task is woken while
  every CPU is busy. 0018 interrupts the busy CPU whose thread goes last,
  but the busy CPUs are always queued for the lock, and whichever chooses
  first takes the root (70 % of boots; the claim is not binding). The
  client then starts where the root is.
- **Placement no longer explains most of the spread** (5-52 % of it), and
  with it held fixed boots still differ by an SD of 1.9-3.1 %. Making the
  claim binding would not plausibly bring the line under 5 %.
- **The contended line spread 11.3 %** over 20 boots (`8241b56`: 7.9 %).
  It is reported as a worst case, not held to the stability test
  (decision 0019).
- **Cost:** 12 cycles on the passive round trip (1 644 against 1 632) and
  384 with a server with its own context (14 400 against 14 016). Changes
  that add nothing to the timed path move these lines by 2-4 % (code
  layout), so part of this may not be 0018's.
- **Kept for:** one interrupt per thread made ready instead of every idle
  CPU waking to find nothing, a better choice of which thread to preempt,
  and what building it found: a preemption that left a runnable thread
  unclaimed (`joist_sched_on` now sets the flag), and the gaps the review
  closed in the tests and the checkers.
