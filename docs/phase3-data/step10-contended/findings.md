# Why the contended IPC line varies from boot to boot (2026-10-15)

Measurements only. Nothing outside my copy `~/joist-ct` on the build VM was
changed, nothing committed, no system setting touched.

## Answer in short

1. **The variation is set at boot and kept through the run.** Inside one
   boot, after one or two slower opening segments, the 20 segment medians sit
   on a plateau of about ±1 %. That plateau differs by up to ~7 % from one boot
   to the next. Taking more calls per boot would not help: each boot's own
   plateau is what gets measured.
2. **What moves is the cost of each critical section and handover, not the
   queue.** The lock is saturated in every boot. The client finds 3 tickets
   ahead of it in 99.8 % of its acquisitions, so every client round trip waits
   behind 2 × 3 busy holds. The order of the busy CPUs in the queue varies
   (no fixed rotation) but does not move with the p50. Across stats boots the
   p50 moves with the time of the holds and waits:
   - 2 × (client wait + hold): r = +0.79;
   - busy CPUs' p99 wait: +0.84;
   - busy mean wait: +0.73;
   - client hold: +0.66;
   - client `abi_syscall` time: +0.72.

   It does not move with the waiting share (+0.15), with interrupts on host
   CPUs 2-5 (+0.02 / −0.15 / −0.34 in lb-cur / stats / light build), with host frequency,
   or with the uncontended lines. The ruler shows +0.24, +0.61 and −0.31 in
   the three builds: no consistent sign.
3. **About three quarters of the boot-to-boot variance (half the SD) comes
   from where the host puts the guest's RAM.** I backed guest RAM with a file in `/dev/shm` that was kept
   between boots, so the guest got the same host pages each time. Within one
   file, boots then agree to SD 67 cycles (0.7 % for one file). The four
   files' means differ by SD 75. Ordinary boots have SD 204 (CV 2.2 %). The
   remaining, smaller part is not the client's vCPU (it stays with RAM *and*
   vCPU fixed), not interrupts, and not the clock. I could not identify it.
   It is likely other per-boot host placement: KVM's own structures (EPT
   tables, VMCS), which a file can't pin. That is an inference; I did not
   test it.
4. **Under today's conditions the 5 % criterion fails most of the time.**
   - 20 lb-cur boots: 8 864-9 600 cycles, **7.9 %**.
   - 10-boot windows of them (boots 1-10, 6-15, 11-20): 6.8 %, 7.2 %, 7.9 %.
   - Resampling 10 of the 25 ordinary boots: under 5 % in **4 %** of draws.

   benchfix's 4.5-5.2 % was a lucky window of the same distribution: its
   values (8 992-9 472) lie inside mine.
5. **The client's vCPU matters a little and not consistently.** In the stats
   build a client on vCPU 3 has a longer hold (~1 560 against ~1 440 cycles,
   mostly `abi_syscall`). In the light build vCPU 3 boots are the fastest
   (median 9 664 against 9 952 on vCPU 1). With RAM fixed, three boots with the
   client on the same vCPU still spread 3.2 %.

## Set-up

- **Host:** settings checked with benchfix's `tools/check.sh` before and
  after each of the four campaigns. Every check passed: governor
  `performance` on CPUs 2-5, `default_smp_affinity` c3, only the known
  per-CPU queue IRQs on 2-5/8-11, build VM QEMU (pid 9937) on 0,1,6,7.
  Results are in `raw/check-{before,after}-*.txt`.
- **Boots:** lock study's `runiso.sh` command line (vCPU n on host CPU 2+n).
  Each boot waited for the build VM to be under 2 % busy and was kept only if
  it stayed so (benchfix run 2's `campaign.sh`, generalised to two images
  booted alternately with the order swapped each round: `tools/campaign.sh`).
  Every boot was kept (VM 0.2-0.5 % busy); none had to be repeated.
  `/proc/interrupts` was captured before and after each boot, and the host
  frequency sampled every 0.2 s.
- **Tree:** `~/joist-ct` = `cp -a ~/joist-f67`, build dirs removed;
  `diff -r -x build` was empty. Then my additions, all inside
  `#ifdef RUN_LOCK_STATS`, `RUN_LOCK_ORDER` or `BENCH_LOCK_STATS`
  (`tools/joist-ct.patch`):
  - stats build: the lock's predecessor CPU and queue depth at asking
    (`klock.last` sits in the owner's cache line);
  - ipcbench: each segment's p50/p99 for the contended line, printed after
    the measurement;
  - a new light build, `RUN_LOCK_ORDER`: the same order counters with **no
    TSC reads**.

  Checks:
  - `lb-cur`'s kernel `.text` and its ipcbench/benchroot code are
    byte-identical to a build of the untouched tree.
  - The stats kernel's `run.o` `.text` is unchanged by the later
    `RUN_LOCK_ORDER` edit.

| Image | KEXTRA | UEXTRA | Boots |
|---|---|---|---|
| `lb-cur` | (none) | (none) | 20 (+5 `anon` controls, +20 file-backed) |
| `lb-cur-stats` | `-DRUN_LOCK_STATS` | `-DBENCH_LOCK_STATS` | 10 |
| `lb-cur-order` (added) | `-DRUN_LOCK_ORDER` | `-DBENCH_LOCK_STATS` | 20 (+12 file-backed) |

All are `ROOT_TASK=benchroot MODULES=ipcbench`, default `BENCH_CALLS`
(20 × 5 000). SHA-256s are in `raw/images.sha256`.

**Units:** stats figures are TSC counts converted to CPU cycles with the
contended line's ruler (12 000 / mean of its min and max counts), as the lock
study's `parse.py` did. ipcbench's p50 has 32-cycle buckets for this line.

**Note: the timer write is now after the unlock** (`RUN_LOCK_TIMER_OUT=1`,
decision 0017). Its column is outside the hold and is not subtracted. The run
queues' hold and settle (`joist_sched_hold`/`joist_sched_settle`) are not timed
separately. The settle on the syscall path is in *the rest*. Settles inside
`abi_syscall` (ipc.c) are in `abi_syscall`.

## Every line, both 10/20-boot sets

| line | lb-cur p50 | lb-cur p99 | lb-cur-stats p50 | lb-cur-stats p99 |
|---|---|---|---|---|
| null call | 530 / 528-552 / 4.5 % | 576 / 576-596 / 3.5 % | 904 / 904-908 / 0.4 % | 948 / 944-976 / 3.4 % |
| 2 AS passive | 1624 / 1624-1628 / 0.2 % | 1824 / 1712-1832 / 6.6 % | 2224 / 2224-2224 / 0.0 % | 2468 / 2336-2504 / 6.8 % |
| 1 AS passive | 1168 / 1168-1172 / 0.3 % | 1232 / 1180-1312 / 10.7 % | 1620 / 1616-1624 / 0.5 % | 1628 / 1628-1736 / 6.6 % |
| 2 AS own ctx | 14048 / 13856-14144 / 2.1 % | 19728 / 19392-20160 / 3.9 % | 15136 / 15040-15232 / 1.3 % | 22160 / 20768-24384 / 16.3 % |
| 2 AS 4 KiB | 2976 / 2976-2984 / 0.3 % | 3116 / 3000-3272 / 8.7 % | 3576 / 3576-3576 / 0.0 % | 3600 / 3592-5520 / 53.6 % |
| contended | 9312 / 8864-9600 / 7.9 % | 10320 / 9664-11744 / 20.2 % | 13552 / 13152-13856 / 5.2 % | 20544 / 14624-27328 / 61.8 % |

The uncontended p50s vary 0.2-2.1 %. The null call's 4.5 % is one boot (552;
the other 19 are 528-532). The contended p50 varies 7.9 %. In the stats build
it varies 5.2 %. The instrumentation lengthens every hold, and that slightly
damps the effect.

## 1. The contended window, stats build, per boot

| boot | p50 | p99 | client vCPU (host CPU) | waiting share | lock held share | free gap per handover | client hold mean / p99 | client wait mean / p99 | busy hold mean / p99 | busy wait mean / p99 | 2 x (client wait + hold) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 6 | 13152 | 20288 | 1 (3) | 74.2 % | 81.9 % | 315 | 1433 / 1601 | 5019 / 5436 | 1415 / 1631 | 5192 / 5608 | 12905 |
| 9 | 13280 | 21984 | 0 (2) | 74.2 % | 82.2 % | 311 | 1448 / 1616 | 5146 / 5547 | 1427 / 1625 | 5180 / 5598 | 13187 |
| 8 | 13472 | 20512 | 1 (3) | 74.3 % | 81.9 % | 320 | 1446 / 1601 | 5145 / 5630 | 1448 / 1700 | 5288 / 5759 | 13182 |
| 2 | 13536 | 20352 | 3 (5) | 74.5 % | 81.1 % | 335 | 1578 / 1889 | 5119 / 5692 | 1389 / 1634 | 5329 / 6015 | 13394 |
| 5 | 13536 | 14624 | 3 (5) | 74.5 % | 81.2 % | 334 | 1560 / 1833 | 5150 / 5680 | 1401 / 1639 | 5336 / 5916 | 13422 |
| 3 | 13568 | 19296 | 1 (3) | 74.5 % | 81.0 % | 339 | 1461 / 1730 | 5191 / 5772 | 1437 / 1705 | 5356 / 5949 | 13304 |
| 7 | 13568 | 20576 | 1 (3) | 74.5 % | 81.1 % | 337 | 1447 / 1665 | 5221 / 5771 | 1446 / 1700 | 5348 / 5900 | 13336 |
| 1 | 13664 | 22336 | 3 (5) | 74.5 % | 81.4 % | 325 | 1547 / 1759 | 5051 / 5516 | 1378 / 1629 | 5251 / 5775 | 13195 |
| 4 | 13664 | 20800 | 3 (5) | 74.5 % | 81.0 % | 341 | 1577 / 1834 | 5215 / 5567 | 1414 / 1614 | 5396 / 5889 | 13582 |
| 10 | 13856 | 27328 | 2 (4) | 74.0 % | 82.1 % | 321 | 1557 / 2012 | 5184 / 6124 | 1439 / 1844 | 5335 / 6376 | 13483 |

*Waiting share* = all CPUs' time in `lock()` / (window × 4). *Lock held
share* = all hold / window. *Free gap* = (window − all hold) / acquisitions:
the time between one holder's release and the next holder's timestamp, i.e.
the handover. *busy* = the three CPUs running the `cap_delete` loop, pooled.
Their p99 is the mean of the three per-CPU p99s.

The hold's parts, per acquisition (CPU cycles):

| boot | p50 | who | hold | abi_syscall | pick | next_event | poke | CR3 | tick | the rest | (timer write, after unlock) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 6 | 13152 | client | 1433 | 513 | 0 | 0 | 0 | 360 | 0.1 | 560 | 2.2 |
| 6 | 13152 | busy (3) | 1415 | 141 | 193 | 353 | 65 | 0 | 0.1 | 663 | 2.1 |
| 9 | 13280 | client | 1448 | 490 | 0 | 0 | 0 | 356 | 0.1 | 602 | 2.7 |
| 9 | 13280 | busy (3) | 1427 | 131 | 195 | 359 | 64 | 0 | 0.1 | 677 | 2.2 |
| 8 | 13472 | client | 1446 | 503 | 0 | 0 | 0 | 367 | 0.1 | 576 | 2.4 |
| 8 | 13472 | busy (3) | 1448 | 140 | 197 | 356 | 65 | 0 | 0.1 | 689 | 2.2 |
| 2 | 13536 | client | 1578 | 581 | 0 | 0 | 0 | 386 | 0.1 | 610 | 2.3 |
| 2 | 13536 | busy (3) | 1389 | 128 | 191 | 354 | 64 | 0 | 0.1 | 651 | 2.5 |
| 5 | 13536 | client | 1560 | 564 | 0 | 0 | 0 | 381 | 0.1 | 615 | 2.7 |
| 5 | 13536 | busy (3) | 1401 | 125 | 194 | 358 | 65 | 0 | 0.1 | 660 | 2.4 |
| 3 | 13568 | client | 1461 | 520 | 0 | 0 | 0 | 365 | 0.1 | 575 | 2.5 |
| 3 | 13568 | busy (3) | 1437 | 140 | 200 | 369 | 64 | 0 | 0.1 | 665 | 2.4 |
| 7 | 13568 | client | 1447 | 519 | 0 | 0 | 0 | 362 | 0.1 | 566 | 2.6 |
| 7 | 13568 | busy (3) | 1446 | 141 | 201 | 355 | 65 | 0 | 0.1 | 684 | 2.4 |
| 1 | 13664 | client | 1547 | 579 | 0 | 0 | 0 | 362 | 0.1 | 605 | 2.4 |
| 1 | 13664 | busy (3) | 1378 | 129 | 190 | 350 | 62 | 0 | 0.1 | 647 | 2.2 |
| 4 | 13664 | client | 1577 | 576 | 0 | 0 | 0 | 375 | 0.1 | 624 | 2.4 |
| 4 | 13664 | busy (3) | 1414 | 123 | 196 | 367 | 65 | 0 | 0.1 | 663 | 2.5 |
| 10 | 13856 | client | 1557 | 583 | 0 | 0 | 0 | 355 | 0.2 | 618 | 3.2 |
| 10 | 13856 | busy (3) | 1439 | 137 | 200 | 372 | 66 | 0 | 0.2 | 664 | 4.2 |

Compared with the lock study's table:
- The busy CPUs' slow path is now pick 190-200, next_event **350-372** (the
  largest scan; the lock study had 146), poke ~64.
- `abi_syscall` is 123-141 for a refused `cap_delete`.
- The rest is ~650-690. It includes entry, charge, `joist_sched_settle`,
  register copies and the instrumentation.
- The timer is almost never written (2-4 cycles averaged per acquisition), and
  after the unlock.
- The client is all fast path: `abi_syscall` 490-583, CR3 355-386, rest
  560-625.

Pick, next_event and poke together are ~43 % of a busy CPU's hold.

## 2. What moves with the contended p50

| figure | r | min-max over boots |
|---|---|---|
| ruler counts, mean of min and max | +0.61 | 7421-7641 |
| ruler max - min | +0.61 | 354-794 |
| IRQs on host CPUs 2-5 | -0.15 | 4283-4770 |
| host freq CPU2-5 mean (MHz) | +0.36 | 2494-4211 |
| null call p50 | +0.01 | 904-908 |
| 2 AS passive p50 | +nan | 2224-2224 |
| own ctx p50 | -0.68 | 1.504e+04-1.523e+04 |
| contended p99 | +0.39 | 1.462e+04-2.733e+04 |
| client vCPU | +0.56 | 0-3 |
| waiting share % | +0.15 | 74.03-74.55 |
| lock held share % | -0.30 | 80.96-82.15 |
| free gap per handover | +0.49 | 311-341.3 |
| 2 x (client wait + hold) | +0.79 | 1.29e+04-1.358e+04 |
| client hold mean | +0.66 | 1433-1578 |
| client hold p99 | +0.79 | 1601-2012 |
| client wait mean | +0.52 | 5019-5221 |
| client wait p99 | +0.71 | 5436-6124 |
| busy hold mean | -0.01 | 1378-1448 |
| busy hold p99 | +0.56 | 1614-1844 |
| busy wait mean | +0.73 | 5180-5396 |
| busy wait p99 | +0.84 | 5598-6376 |
| client sys | +0.72 | 489.5-583.1 |
| client pick | +0.09 | 0.09719-0.2059 |
| client next | +0.48 | 0.05299-0.0907 |
| client poke | +0.69 | 0.104-0.2102 |
| client cr3 | +0.10 | 355.3-386.3 |
| client rest | +0.57 | 560.1-624.4 |
| client timer | +0.59 | 2.183-3.211 |
| busy sys | -0.24 | 122.9-141.4 |
| busy pick | +0.29 | 189.8-200.8 |
| busy next | +0.52 | 350.2-372.4 |
| busy poke | -0.00 | 62.46-65.57 |
| busy cr3 | +nan | 0-0 |
| busy rest | -0.25 | 646.9-689.1 |
| busy timer | +0.70 | 2.111-4.221 |

| figure | r | min-max over boots |
|---|---|---|
| ruler counts, mean of min and max | +0.24 | 7424-7523 |
| ruler max - min | -0.12 | 202-474 |
| IRQs on host CPUs 2-5 | +0.02 | 3366-4676 |
| host freq CPU2-5 mean (MHz) | -0.04 | 2106-4246 |
| null call p50 | +0.02 | 528-552 |
| 2 AS passive p50 | -0.21 | 1624-1628 |
| own ctx p50 | +0.10 | 1.386e+04-1.414e+04 |
| contended p99 | +0.57 | 9664-1.174e+04 |

| figure | r | min-max over boots |
|---|---|---|
| ruler counts, mean of min and max | -0.31 | 7426-7522 |
| ruler max - min | +0.25 | 206-396 |
| IRQs on host CPUs 2-5 | -0.34 | 3671-4481 |
| host freq CPU2-5 mean (MHz) | +0.29 | 2087-4188 |
| null call p50 | -0.12 | 536-544 |
| 2 AS passive p50 | -0.01 | 1636-1640 |
| own ctx p50 | +0.05 | 1.386e+04-1.43e+04 |
| contended p99 | +0.62 | 1.04e+04-1.584e+04 |
| client vCPU | -0.45 | 0-3 |
| client vCPU is 3 | -0.49 | 0-1 |
| client depth 3 share | +0.47 | 99.81-99.87 |
| client depth <3 share | -0.47 | 0.129-0.1855 |
| busy depth 3 share | +0.53 | 98.16-98.37 |
| busy acquisitions, total | -0.55 | 6.091e+05-6.104e+05 |
| busy acquisitions per client acquisition | -0.54 | 3.046-3.052 |
| busy largest predecessor share, mean | +0.03 | 35.68-45.3 |

lb-cur boot *k* and stats boot *k* were booted back to back, but they are
different boots. A shared per-boot cause cannot be paired this way:
r(lb-cur p50, stats p50) = +0.02. The figures below are therefore chance or
slow drift, and I don't read anything into them:

- r(lb-cur p50, stats p50) = +0.02
- r(lb-cur p50, stats 2 x (client wait + hold)) = -0.08
- r(lb-cur p50, stats busy hold mean) = +0.84
- r(lb-cur p50, stats free gap per handover) = -0.19
- r(lb-cur p50, stats client vCPU) = -0.76

**Reading.** Ten boots give r only roughly (|r| < 0.63 is not significant at
5 % for 10 boots; < 0.44 for 20). The pattern is still consistent:
- The p50 follows how long the holds and waits take: client hold and
  `abi_syscall`, busy p99 hold, all waits.
- It does not follow how much waiting there is: the waiting share is a flat
  74.0-74.5 % and the queue is always full.
- In a saturated FIFO lock, the client's round trip ≈ 2 × (3 busy holds + its
  own hold + 4 handovers) + user time, so every cycle a critical section or a
  handover costs is multiplied by ~8 in the p50.
- Kernel figures that are not cache-sensitive (tick, poke counts, the timer)
  show nothing.
- No exact per-boot decomposition comes out. Mean figures over the whole
  window can't capture a per-boot shift of ~150 cycles in an 8-hold round
  trip (~20 cycles per hold) once the instrumentation's own noise is added.

## 3. Other differences between fast and slow boots

**Plateau, not noise.** Segment p50s of the stats build, boot by boot (20
segments of 5 000 calls). Segment 1, sometimes also segment 2, is 3-8 % slow;
the rest is flat:

| boot | p50 | segments 1-20 |
|---|---|---|
| 6 | 13152 | 13696 13568 13376 13088 13120 13152 13152 13184 12992 13184 13184 12992 13184 12992 12992 13088 12992 13024 13152 13216 |
| 9 | 13280 | 14208 13536 13280 13120 13088 13312 13248 13312 13152 13152 13312 13184 13376 13312 13216 13312 13440 13216 13248 13280 |
| 8 | 13472 | 14144 13600 13376 13312 13504 13472 13344 13440 13440 13344 13472 13408 13504 13664 13408 13472 13344 13536 13376 13504 |
| 2 | 13536 | 14080 14016 14496 13440 13760 13344 13312 13344 13248 13248 13632 13376 13440 13504 13632 13632 13536 13664 13504 13600 |
| 5 | 13536 | 14528 13824 13472 13568 13568 13504 13472 13536 13472 13472 13536 13632 13632 13504 13632 13632 13504 13536 13568 13536 |
| 3 | 13568 | 14304 13760 13792 13536 13440 13568 13632 13600 13600 13504 13600 13536 13376 13472 13536 13568 13568 13344 13472 13600 |
| 7 | 13568 | 14464 14368 13696 13344 13536 13696 13408 13664 13408 13472 13408 13536 13568 13600 13568 13600 13376 13632 13408 13504 |
| 1 | 13664 | 14144 13760 13920 13568 13376 14048 13920 13952 13664 13760 13504 13152 13504 13440 13728 13696 13312 13408 13664 13504 |
| 4 | 13664 | 13888 13792 13888 13664 13664 13568 13664 13600 13600 13568 13600 13696 13600 13664 13696 13664 13568 13632 13568 13600 |
| 10 | 13856 | 15072 13696 14016 13952 13280 13568 13888 13440 13856 14112 13632 13472 14080 13568 13376 14080 13632 13728 14112 14048 |

The lb-cur `worst seg p50` column (below) shows the same: the worst segment
is 2-7 % above the median in every boot, whether the boot is fast or slow.

**The client's vCPU.** Global scheduling picks it at the start of the
measurement, so it varies by boot:
- stats: vCPU 1 four times, vCPU 3 four times, 0 and 2 once each;
- order build: vCPU 1 seven times, vCPU 3 seven, vCPU 2 four, vCPU 0 twice.

| client vCPU | boots | p50 each | median |
|---|---|---|---|
| 0 | 2 | 9824, 9856 | 9840 |
| 1 | 7 | 9600, 9824, 9888, 9952, 10016, 10048, 10432 | 9952 |
| 2 | 4 | 9600, 9664, 9888, 10208 | 9776 |
| 3 | 7 | 9504, 9536, 9632, 9664, 9664, 9792, 9920 | 9664 |

| client vCPU | boots | p50 each | median |
|---|---|---|---|
| 0 | 1 | 13280 | 13280 |
| 1 | 4 | 13152, 13472, 13568, 13568 | 13520 |
| 2 | 1 | 13856 | 13856 |
| 3 | 4 | 13536, 13536, 13664, 13664 | 13600 |

So vCPU 3 is the fastest client CPU in the light build and among the slower
ones in the stats build. The client's hold is ~120 cycles longer there
(abi_syscall ~575 against ~505, CR3 ~375 against ~360). It is an effect of
up to ~3 %, not a stable ordering. lb-cur itself can't show the client's
vCPU.

**Lock order (light build, no TSC).** Busy CPUs make 3.05 acquisitions per
client acquisition. The client finds 3 tickets ahead in 99.8-99.9 % of
acquisitions, the busy CPUs in 98.2-98.4 %. Each CPU's predecessor is spread
over the other three: the largest share is 34-52 %, so there is no fixed
rotation. None of this differs between fast and slow boots: r with depth
share is ±0.5 over a 0.13-0.19 % range; with the predecessor share, +0.03.

| boot | p50 | p99 | client vCPU (host CPU) | client acq | busy acq each | client depth 0/1/2/3 (%) | busy depth 0/1/2/3 (%) | client's predecessor CPU 0/1/2/3 (%) | busy: largest predecessor share each (%) | segment p50s min-median-max | ruler min-max | IRQs host CPUs 2/3/4/5 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 11 | 9504 | 10400 | 3 (5) | 200010 | 203324/203328/203320 | 0.0/0.0/0.2/99.8 | 0.0/0.0/1.8/98.2 | 42/27/31/0 | 38/37/40 | 9280-9488-10496 | 7416-7626 | 1340/671/1146/630 |
| 7 | 9536 | 10880 | 3 (5) | 200002 | 203266/203266/203261 | 0.0/0.0/0.1/99.8 | 0.0/0.0/1.7/98.3 | 43/23/34/0 | 39/39/38 | 9376-9520-10080 | 7244-7640 | 1658/1133/781/744 |
| 9 | 9600 | 10784 | 2 (4) | 200005 | 203480/203475/203454 | 0.0/0.0/0.2/99.8 | 0.0/0.0/1.8/98.2 | 36/22/0/42 | 34/40/45 | 9376-9584-10432 | 7416-7622 | 1767/783/972/652 |
| 18 | 9600 | 10880 | 1 (3) | 200009 | 203324/203322/203320 | 0.0/0.0/0.2/99.8 | 0.0/0.0/1.8/98.2 | 35/0/26/39 | 42/42/37 | 9472-9600-10336 | 7244-7632 | 1345/561/712/1065 |
| 16 | 9632 | 11040 | 3 (5) | 200002 | 203246/203242/203230 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.7/98.3 | 36/23/41/0 | 52/40/43 | 9376-9600-10880 | 7244-7626 | 1479/579/931/712 |
| 1 | 9664 | 11424 | 3 (5) | 200010 | 203260/203273/203255 | 0.0/0.0/0.2/99.8 | 0.0/0.0/1.7/98.2 | 41/25/34/0 | 42/42/43 | 9440-9648-11040 | 7244-7614 | 1440/1064/625/1171 |
| 12 | 9664 | 10944 | 3 (5) | 200004 | 203257/203262/203252 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.7/98.3 | 41/23/36/0 | 36/37/41 | 9408-9632-10976 | 7244-7622 | 1440/1317/1071/653 |
| 17 | 9664 | 10752 | 2 (4) | 200004 | 203453/203459/203439 | 0.0/0.0/0.2/99.8 | 0.0/0.0/1.8/98.2 | 38/29/0/33 | 37/38/39 | 9472-9664-10464 | 7244-7630 | 1564/735/1028/720 |
| 15 | 9792 | 11328 | 3 (5) | 200001 | 203088/203087/203088 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.6/98.4 | 37/28/35/0 | 39/37/37 | 9632-9792-10592 | 7266-7608 | 1564/795/899/654 |
| 6 | 9824 | 11168 | 1 (3) | 200003 | 203197/203200/203193 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.7/98.3 | 39/0/32/29 | 36/41/36 | 9600-9824-10592 | 7244-7626 | 1440/1118/773/729 |
| 8 | 9824 | 10944 | 0 (2) | 200004 | 203042/203042/203036 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.6/98.4 | 0/38/34/28 | 44/36/37 | 9600-9808-10336 | 7244-7614 | 1031/738/1595/695 |
| 5 | 9856 | 11008 | 0 (2) | 200010 | 203092/203084/203092 | 0.0/0.0/0.2/99.8 | 0.0/0.0/1.7/98.3 | 0/37/38/26 | 41/39/41 | 9504-9728-10496 | 7244-7630 | 1381/824/850/616 |
| 2 | 9888 | 11360 | 2 (4) | 200001 | 203316/203318/203311 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.8/98.2 | 36/31/0/32 | 42/43/43 | 9504-9856-10624 | 7244-7640 | 1490/886/651/720 |
| 3 | 9888 | 12288 | 1 (3) | 200004 | 203269/203260/203251 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.7/98.3 | 32/0/30/38 | 41/39/38 | 9536-9872-10848 | 7244-7626 | 1610/697/635/861 |
| 19 | 9920 | 11072 | 3 (5) | 200020 | 203121/203129/203114 | 0.0/0.0/0.1/99.8 | 0.0/0.0/1.7/98.3 | 39/27/35/0 | 37/37/37 | 9728-9904-10624 | 7416-7628 | 1286/757/991/719 |
| 4 | 9952 | 11296 | 1 (3) | 200007 | 203147/203137/203146 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.7/98.3 | 38/0/31/30 | 38/37/38 | 9728-9936-10496 | 7244-7624 | 1432/1098/804/791 |
| 14 | 10016 | 11616 | 1 (3) | 200003 | 203149/203146/203131 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.7/98.3 | 39/0/30/31 | 35/35/37 | 9600-9952-11008 | 7244-7608 | 1456/841/702/856 |
| 13 | 10048 | 11648 | 1 (3) | 200005 | 203099/203086/203097 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.7/98.3 | 37/0/29/35 | 43/38/36 | 9888-10032-10912 | 7244-7618 | 1515/759/741/763 |
| 20 | 10208 | 15840 | 2 (4) | 200002 | 203219/203233/203209 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.7/98.3 | 39/23/0/38 | 36/45/45 | 9760-10160-10976 | 7244-7626 | 1460/770/772/795 |
| 10 | 10432 | 11936 | 1 (3) | 200004 | 203140/203120/203125 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.7/98.3 | 45/0/26/29 | 43/38/44 | 10336-10432-10816 | 7244-7620 | 906/1041/1196/696 |

**Ruler, host frequency, interrupts (lb-cur, 20 boots).**
- Ruler: the minimum is always 7 242-7 246 or 7 416 counts (two turbo
  steps), and the maximum ~7 600-7 720 in every boot.
- Host CPUs 2-5 got 3 366-4 676 interrupts per boot, ~90 % of them the host's
  LOC tick, then CAL/TLB shootdowns. These are the same sources as benchfix
  run 2.
- A host CPU's median frequency of 800 MHz only means that vCPU idled in the
  uncontended phases.
- None of these follows the p50.

| boot | contended p50 | p99 | worst seg p50 | ruler counts min-max | host MHz CPU2/3/4/5 (median) | IRQs on host CPUs 2/3/4/5 | top sources |
|---|---|---|---|---|---|---|---|
| 13 | 8864 | 9664 | 9088 | 7242-7616 | 2450/800/4264/4296 | 1156/701/878/1282 | LOC 3591, CAL 256, TLB 146 |
| 5 | 8896 | 9760 | 9280 | 7416-7624 | 4336/4120/4120/4120 | 1344/944/696/720 | LOC 3506, CAL 124, TLB 62 |
| 16 | 8992 | 10144 | 9408 | 7244-7616 | 4248/800/2450/4260 | 1177/589/1018/842 | LOC 3505, CAL 73, TLB 32 |
| 20 | 9024 | 9952 | 9472 | 7416-7630 | 4250/3857/800/2450 | 1333/779/714/979 | LOC 3400, CAL 254, TLB 130 |
| 1 | 9056 | 10336 | 9696 | 7244-7628 | 4279/4161/4205/800 | 1611/1121/999/581 | LOC 3911, CAL 255, TLB 126 |
| 2 | 9184 | 11008 | 9664 | 7244-7628 | 4306/4194/4203/4266 | 1502/987/733/988 | LOC 4062, CAL 98, TLB 32 |
| 4 | 9216 | 10016 | 9760 | 7246-7624 | 4269/4099/4109/4246 | 1343/737/743/836 | LOC 3544, CAL 72, TLB 30 |
| 11 | 9216 | 9952 | 9792 | 7416-7628 | 4116/4239/4103/4163 | 1366/1027/672/1092 | LOC 3924, CAL 126, TLB 75 |
| 6 | 9280 | 10208 | 9536 | 7244-7622 | 4276/4145/4221/4145 | 1115/1145/959/757 | LOC 3687, CAL 162, TLB 77 |
| 14 | 9312 | 10432 | 9792 | 7244-7624 | 4158/4247/4266/4108 | 915/1284/1119/1358 | LOC 4312, CAL 217, TLB 124 |
| 15 | 9312 | 10112 | 9600 | 7244-7630 | 4151/2450/2450/4203 | 1122/1272/1148/1051 | LOC 4180, CAL 239, TLB 146 |
| 12 | 9376 | 10560 | 9888 | 7244-7626 | 4280/4104/800/2450 | 1440/549/646/866 | LOC 3326, CAL 108, TLB 47 |
| 8 | 9408 | 10272 | 9760 | 7416-7628 | 4185/4127/4186/4148 | 1284/835/808/617 | LOC 3272, CAL 161, TLB 93 |
| 17 | 9408 | 10304 | 9824 | 7244-7604 | 4376/2450/800/800 | 1203/689/706/768 | LOC 3223, CAL 84, TLB 46 |
| 9 | 9472 | 10336 | 9728 | 7416-7618 | 4288/4152/4290/4252 | 1483/1033/981/924 | LOC 4180, CAL 119, TLB 78 |
| 18 | 9504 | 11744 | 9888 | 7242-7612 | 4257/4133/800/4260 | 1432/646/776/1559 | LOC 3964, CAL 256, TLB 169 |
| 3 | 9536 | 10432 | 9888 | 7416-7626 | 4310/4119/2450/800 | 1495/659/724/538 | LOC 3276, CAL 85, TLB 39 |
| 7 | 9536 | 10368 | 10144 | 7244-7718 | 4321/4193/800/800 | 1825/924/626/567 | LOC 3561, CAL 232, TLB 118 |
| 10 | 9536 | 10368 | 9888 | 7416-7622 | 4269/4122/4239/4122 | 903/811/805/1370 | LOC 3706, CAL 109, TLB 57 |
| 19 | 9600 | 10656 | 10112 | 7416-7630 | 4286/4122/800/4265 | 1692/640/549/1137 | LOC 3680, CAL 204, TLB 111 |

## 4. The placement test

`lb-cur.iso` was booted with guest RAM in a file on `/dev/shm` (QEMU
`memory-backend-file,share=on`, `tools/runiso-mem.sh`), kept between boots, so
each boot ran on the same host pages. Four files, A-D, 5 boots each,
interleaved round by round with an ordinary-RAM control (`anon`). The files
were deleted afterwards.

| RAM | boots | contended p50, boot by boot | median | spread within | contended p99 median | null call p50 range | 2 AS passive p50 range | own ctx p50 range |
|---|---|---|---|---|---|---|---|---|
| memA | 5 | 9152, 9216, 9056, 9152, 9216 | 9152 | 1.7 % | 10144 | 528-528 | 1624-1628 | 13984-14144 |
| memB | 5 | 9152, 9088, 9088, 9120, 9088 | 9088 | 0.7 % | 9984 | 528-532 | 1624-1628 | 14048-14304 |
| memC | 5 | 9568, 9344, 9344, 9344, 9152 | 9344 | 4.5 % | 10304 | 528-532 | 1624-1624 | 13984-14112 |
| memD | 5 | 9184, 9248, 9344, 9120, 9216 | 9216 | 2.4 % | 10848 | 528-532 | 1624-1628 | 14048-14304 |
| anon | 5 | 9216, 9216, 9056, 9312, 9408 | 9216 | 3.8 % | 10240 | 528-528 | 1624-1628 | 14016-14272 |

- spread of the four files' medians: 9088-9344, 2.8 %
- all file-backed boots together: 9056-9568, spread 5.6 %

- **Each file's first boot** is the one that allocates the pages (memC's
  9 568). Without it, within-file SD is **67** cycles. The four files' means
  have SD **75**. Ordinary boots (20 + 5) have **204**. So fixed placement
  removes about three quarters of the variance, and what is left is half
  within a placement and half between placements.
- Huge pages are not the switch: `AnonHugePages` was 18 MB in every ordinary
  boot and 6 MB in every file-backed boot (4 KiB shmem pages), with no link to
  the p50.
- The uncontended lines are unchanged by file backing.

The light build on two kept files, with the client's vCPU:

| RAM | boots | p50 (client vCPU), boot by boot | spread | median by client vCPU |
|---|---|---|---|---|
| memE | 6 | 10272 (3), 9920 (1), 9792 (3), 9952 (1), 10016 (0), 9696 (3) | 5.8 % | vCPU 0: 10016 (1), vCPU 1: 9936 (2), vCPU 3: 9792 (3) |
| memF | 6 | 10080 (2), 9824 (2), 9888 (1), 9728 (0), 10080 (2), 9760 (2) | 3.6 % | vCPU 0: 9728 (1), vCPU 1: 9888 (1), vCPU 2: 9952 (4) |

In this build, each file's first boot is again the slowest (memF's ties with its 5th). Even with the
same RAM and the same client vCPU (memF, vCPU 2: 9 824, 10 080, 9 760), boots
differ by ~3 %.

## What this means for a kernel change (observations, not a recommendation)

- The contended line's p50 is ~8 critical sections plus handovers. Each is
  dominated by cache lines moving between cores. How fast those move depends
  on where the host put the memory, and that changes at every boot. Any
  per-boot change in line-transfer cost gets multiplied by the length of the
  queue (3 ahead, twice per round trip).
- Two things could cut the boot-to-boot variation in cycles:
  - **less shared data touched per hold**, and **shorter holds** on the busy
    CPUs. Their refused `cap_delete` goes through `schedule()`: pick, poke,
    and above all `next_event`, ~610 of ~1 415 cycles instrumented, every
    time, although the same thread resumes;
  - **fewer holds per round trip**.

  The relative spread falls only if the variable part (line transfers) shrinks
  more than the fixed part.
- The uncontended lines are already stable because they move no lines
  between cores.
- On the measurement side: the plateau is per boot. More calls per boot won't
  narrow it. A median over several boots, or a wider criterion for this one
  line, would. 20 boots of the current kernel give a CV of 2.2 %. With that,
  a 10-boot max-min under 5 % happens ~4 % of the time.

## Files

- `raw/`: every serial log (`IMG-N.log`) with `.freq`, `.irq-before`,
  `.irq-after`; `campaign.log`; `check-*.txt`; `images.sha256`; smoke boots;
  `tables.md` (all tables, `tools/analyze.py` output).
- `iso/`: the three images.
- `tools/`:
  - `campaign.sh`, `campaign-mem.sh`, `runiso-mem.sh`, `check.sh`
    (benchfix's, unchanged), `analyze.py`;
  - `joist-ct.patch`, the whole diff against `~/joist-f67`;
    `stats-only.patch` and `order-only.patch` are the two steps of it.
