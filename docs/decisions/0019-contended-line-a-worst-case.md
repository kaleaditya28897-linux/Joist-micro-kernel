# 0019 — The contended line is a worst case, reported, not held to the stability test

Date: 2026-10-19
Status: **accepted** (2026-10-19). The owner chose it from the options in
the session of that date, after the placement study below.
Covers: Phase 3's exit criterion (*IPC benchmarks stable*), 0016 and 0017
(the one lock), 0018 (wake placement).

## Context

The plan defines *stable* for the exit benchmark (`docs/phase3-plan.md`,
the exit row of the work table): `user/ipcbench`'s round trips, in one
address space and between two, fast and slow path. Across 10 runs the
median must move less than 5 % and the 99th percentile less than 20 %.
The exit benchmark also has the do-nothing call and a server with its own
context.

Step 9 added one more line: one pair's round trip **while every other CPU
does nothing but system calls**. That is the worst case for the one kernel
lock (0016), and it was added to measure the lock's cost, which 0016 said
would be "measured, then revisited". Since step 10 every line was held to
the stability test, this one too. Every other line passes it, in every
10-run window of 20-boot and 40-boot campaigns
(`docs/phase3-data/step10-pokes/`). This one does not:

| Kernel | Median | Spread over 20 boots |
|---|---|---|
| `f67da20` | 9 232 | 7.9-8.5 % |
| `8241b56` (a call that changes nothing skips the scheduler) | 7 056 | 7.9 % |
| 0018 (one CPU woken per thread) | 7 096 | 11.3 % |

Four studies by the P53's Claude Code looked for the cause, about 1 150
boots in all (`docs/phase3-data/step10-contended/`, `step10-contended2/`,
`step10-place0018/`):

- The host was quieted first: governor, interrupts, the build VM and the
  guest's RAM (`docs/testbed.md`, *Benchmark CPUs*). That fixed the other
  lines' misses, not this one.
- The lock is a full convoy: the client is behind all three busy CPUs in
  98.7-99.9 % of its acquisitions.
- Which virtual CPU the client lands on was 47 % of the variance before
  0018. 0018 did not fix it: the root task is woken while every CPU is
  busy, and whichever busy CPU reaches the lock first takes it, not the one
  0018 interrupted (70 % of boots). Now placement is 5-52 % of the
  variance.
- With placement held fixed, boot medians still differ by an SD of
  1.9-3.1 %. Passing every 10-run window of a 20-boot campaign reliably
  needs about 1.2 %. Measurements in the same boot differ by about as much
  as boots do.
- Kernel changes that add nothing to the timed path move the line's level
  by 2-4 %: code layout.
- No host figure tracks it: host core, clock, temperature, host
  interrupts. What can't be tested without root (KVM's own page
  placement) can't explain the measurements that differ within one boot.
- Measuring the line three times per boot and taking the median spread
  9.0 %.

## Options considered

1. **Report the line as a worst case, not held to the test** (this
   decision).
2. **Keep looking:** host causes that need root, then a CPU affinity call
   so the benchmark can pin its threads. Days of work, with no strong lead.
3. **Hold the exit until bare metal**, the line part of the criterion.

## Decision

The stability test applies to the lines the plan names, and to the
do-nothing call and the server with its own context, as before. The
contended line is published in every benchmark record with its median, its
99th percentile and its spread, and the studies that explain it. It is not
held to the 5 % / 20 % test. `docs/phase3-data/step10-stays/analyze.py`
marks it `worst case` and leaves it out of the verdict.

The line is still measured in every exit campaign. It is reopened:
- on bare metal, when the testbed has it;
- when the lock changes (0017's trigger: an MCS lock, several locks, or
  any change to who takes the lock and when);
- if its median rises more than 25 % above `8241b56`'s 7 056 cycles.

## Consequences

- Phase 3's benchmark criterion is met: every held line passes in every
  10-run window.
- What the line measures is still the most useful figure about the one
  lock under load, and it stays in view: its 7 056-cycle median (9 232
  before `8241b56`) is 4.3 times the uncontended round trip.
- The cause of the spread stays unexplained. The studies narrow it to a
  level set per boot and per measurement, sensitive to code layout, and not
  the lock queue or the host's interrupts or clock. That is recorded, not
  fixed.

## Priority check

Correctness is unaffected: this decides what a benchmark is held to, not
what the kernel does. The plan's own criterion is met as written. The
contended line was never in it, and changing that would hold the exit to a
figure that four studies could not make stable on this testbed.
