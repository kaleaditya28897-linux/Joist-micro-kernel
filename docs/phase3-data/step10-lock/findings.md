# Lock study: measurements for revisiting decision 0016 (2026-10-15)

Measurements only. What follows is what was measured, how, and what could
not be measured. Whether 0016 should change is not discussed here.

## Set-up

- **Host:** the P53, Intel Core i7-9850H, Linux 6.18.56_1, governor
  `powersave` (intel_pstate, turbo on). **QEMU 11.1.2**, KVM, `-machine
  q35,accel=kvm -cpu host -smp 4 -m 256M`, SeaBIOS. vCPU *n* pinned to
  host CPU 2+*n* (2, 3, 4, 5: four cores whose second threads 8 to 11
  nothing of ours used), under `flock /srv/joist/test/lock`, the same
  command line as `tools/p53/boot.sh` (`tools/runiso.sh`). Every log
  records the pinning (`pin:` lines).
- **Quiet machine:** the build VM (unpinned, 8 vCPUs) was not idle much
  of the afternoon: the other session ran model tests and self-tests in
  `~/joist-rq`. Each boot therefore waited until the build VM's
  `/proc/stat` showed under 2 % busy over 5 s. It was **kept only if the
  build VM stayed under 2 % busy over the whole boot**, and repeated
  otherwise (`tools/campaign.sh`). 6 boots were discarded and repeated.
  Each kept log's first line gives the VM's busy share during it (0.0 to
  1.5 %).
- **Tree:** a copy of `~/joist` taken after the full run finished
  (`~/joist-lock`), plus `patch.diff`. The full run itself ended
  `FULL RUN: FAILED: model/mutate kernel/check` (`kernel/check`: the
  self-test `chk-kvm-smp2-pingpong-buf` timed out, status 124). That run
  is not mine and I did not look into it; it is noted because these
  measurements are of that tree.
- **Runs:** each variant booted 7 or more times (16 for the plain ticket
  kernel, which also served as a control between later variants),
  variants interleaved round by round. Figures are **median (min-max)
  over boots**. Raw serial logs are in `raw/`; `analysis.txt` is
  `tools/parse.py`'s output over all of them. Image SHA-256s are in
  `raw/images.sha256`.
- **Units.** The TSC runs at 2 592.5 MHz (scaleroot measures it against
  the kernel's clock at every boot: 2 592 451 to 2 592 675 counts/ms). The
  CPU's own clock varies with turbo, so **CPU cycles** are converted from
  TSC counts with ipcbench's ruler (a dependent chain of 4 000 `imul`,
  12 000 cycles), read by each client during its own window, and averaged
  over the clients of that window. That gave 1.72 CPU cycles per TSC count
  with one pair busy (about 4.45 GHz), falling to 1.61 with four (about
  4.18 GHz). So **throughput per second falls partly because the clock
  does**; the cycles-per-round-trip columns remove that. The kernel's
  lock figures (counted in TSC counts) are converted with the same run's
  ratio.

## Kernel builds (all flags in `patch.diff`)

| Image | KEXTRA | Root task |
|---|---|---|
| `ls-ticket` | (none: the current lock) | scaleroot |
| `ls-mcs` | `-DRUN_LOCK_MCS=1` | scaleroot |
| `ls-timerout` | `-DRUN_LOCK_TIMER_OUT=1` | scaleroot |
| `ls-1cpu` | `-DRUN_MAX_CPUS=1u` (no lock taken) | scaleroot |
| `ls-*-stats` | the above plus `-DRUN_LOCK_STATS` | scaleroot |
| `lb-ticket`, `lb-mcs`, `lb-*-stats` | as above | benchroot + ipcbench, `UEXTRA=-DBENCH_CALLS=100000u` (`-DBENCH_LOCK_STATS` with the stats kernels) |

Built with `make BUILD=build/NAME USER_BUILD=... KEXTRA=... ROOT_TASK=...
MODULES=ipcbench build/NAME/joist.iso` in `~/joist-lock/kernel`. The
non-stats kernels' `.text` is byte-identical whether built from the final
`run.c` or from the earlier one their images came from (checked with
`llvm-objcopy -j .text`): the later edits are all inside `#ifdef`s.

## 1. Scaling of IPC throughput with CPUs

`user/scaleroot` (new) starts K independent pairs, each a client task and
a server task of ipcbench (new role 3: call for a fixed time). All clients
start their 2 s window at the same TSC moment and report their round trips.
The **4-CPU kernel** is used throughout; K = 1 to 4. The server's context
is either none (*passive*: the pair runs on the client's CPU through the
direct switch) or its own, at a priority above its client's
(*own-context*: every call and reply is a change of context and a timer
write). On every run and every K, all pairs got the same share (least to
most pair at least 0.99), and no client started late.

**Plain kernel (ticket lock), 16 boots:**

| Mode | K | Round trips/s, total | Per pair | Against K=1 (ideal K) | CPU cycles per round trip (per pair) |
|---|---|---|---|---|---|
| passive | 1 | 2 909 k (2 779-2 949 k) | 2 909 k | 1.00 | 1 527 (1 513-1 537) |
| passive | 2 | 2 115 k (2 029-2 228 k) | 1 057 k | 0.73 | 4 119 (3 901-4 221) |
| passive | 3 | 1 906 k (1 773-1 981 k) | 635 k | 0.66 | 6 750 (6 482-7 098) |
| passive | 4 | 1 657 k (1 613-1 705 k) | 414 k | 0.57 | 10 102 (9 887-10 462) |
| own-context | 1 | 295 k (290-301 k) | 295 k | 1.00 | 14 969 (14 807-15 105) |
| own-context | 2 | 266 k (258-270 k) | 133 k | 0.90 | 32 746 (31 831-33 183) |
| own-context | 3 | 260 k (255-263 k) | 87 k | 0.88 | 49 634 (48 332-50 042) |
| own-context | 4 | 251 k (245-255 k) | 63 k | 0.85 | 66 766 (65 525-68 231) |

So total throughput does not rise with CPUs in either mode; it falls. Four
pairs together make 57 % (passive) and 85 % (own-context) of what one
pair alone makes.

Reference, the **1-CPU kernel** (no lock taken), K = 1 only, 8 boots:
passive 2 870 k/s, 1 453 (1 427-1 464) cycles per round trip, so the 4-CPU
kernel's uncontended lock costs about 74 cycles per round trip (two kernel
entries). Own-context: 335 k/s, 12 289 cycles. In every boot the 1-CPU
own-context client started after its window had opened, so this figure
undercounts by an unknown amount (a lower bound on throughput). I did not
find why. Passive was not affected.

Throughput in round trips per second is also affected by the CPU's clock,
which falls as more cores are busy (above). The cycle columns don't move
with the clock: per pair, a passive round trip goes from 1 527 cycles to
10 102 at K=4, 6.6 times.

## 2. Where a contended entry's time goes

Kernel built with `-DRUN_LOCK_STATS` (`patch.diff`, `run.c`). On every
acquisition each CPU records the TSC at asking, at getting, and at letting
go (`lfence; rdtsc; lfence`). It keeps, per CPU, sums, counts and
histograms (8-count buckets) of waiting time and holding time. It also
times, inside the hold, `joist_sched_pick_on`, `joist_sched_next_event`,
`poke_others` (all its `joist_sched_better` calls), `joist_tick`,
`abi_syscall`, the timer write and the address-space switch. Only
acquisitions inside a window the root task sets are counted (system call
99, which the ABI then refuses as unknown). Hold times are split by how
the lock was let go: the fast path (direct switch), the slow path
(scheduler), or idle.

*Waiting share* = all CPUs' time in `lock()` / (window × 4 CPUs).
*Lock busy* = all hold time / window (how much of the time somebody held
it). Mean and 99th-percentile **hold and wait per acquisition are in CPU
cycles**, converted as described under *Units*. Medians over boots,
(min-max).

**The instrumentation costs a lot here**, and the absolute hold times
include it. The stats kernel's K=1 passive round trip is 2 398 cycles
against 1 527 uninstrumented (+871, about 435 per kernel entry, most of
it inside the hold). The shares and how hold and wait move with K are the
useful part; the absolute holds are upper bounds. An uninstrumented
cross-check is under the tables.

**Scaling test, ticket lock, 14 boots:**

| | passive K=1 | passive K=4 | own-context K=1 | own-context K=4 |
|---|---|---|---|---|
| Waiting share of all CPU time | 1.7 % | **75.6 %** (75.4-75.7) | 0.2 % | **75.1 %** (75.1-75.1) |
| Lock busy (share of wall time) | 72 % | 79 % | 96 % | 96 % |
| Mean hold, cycles | 865 (859-871) | 1 293 (1 273-1 307) | 8 000 (7 911-8 052) | 8 891 (8 743-9 803) |
| 99th-percentile hold | 921 (909-933) | 1 557 (1 527-1 594) | 11 664 (11 423-11 813) | 12 618 (12 448-20 621) |
| Mean wait | 81 | 4 933 (4 843-4 973) | 83 | 27 722 (27 305-30 550) |
| 99th-percentile wait | 96 | 5 353 (5 287-5 449) | 95 | 42 664 (41 447-53 303) |
| Acquisitions on the fast path | all but ~200 of 7.4 M | all but ~900 of 5.1 M | none | none |

(At K = 2 and 3: waiting 25.5 % and 50.2 % passive, 25.1 % and 50.1 %
own-context; mean hold passive 1 134 and 1 211.)

**ipcbench's contended line** (step 9's measurement: one client and passive
server, every other CPU calling `cap_delete` on nothing as fast as it can).
`lb-ticket-stats`, 7 boots, counted over the 2 M timed calls:

| | |
|---|---|
| Waiting share of all CPU time | 75.4 % (75.1-75.6) |
| Lock busy | 80 % |
| Mean hold / 99th percentile | 1 347 (1 316-1 362) / 1 562 (1 509-1 625) cycles |
| Mean wait / 99th percentile | 5 058 (5 014-5 117) / 5 487 (5 392-5 545) cycles |
| Acquisitions | 16.1 M: 4.0 M fast path (the client), 12.1 M slow path (the three busy CPUs) |

**Uninstrumented cross-check.** At saturation one acquisition follows
another, so wall time per acquisition = 1 / (round trips/s × 2). Plain
kernel, passive K=4: 1 657 k round trips/s is 3.31 M acquisitions/s, one
every **1 261 CPU cycles** (at 1.612 cycles per TSC count). At K=1 a whole
kernel entry plus its share of user time is 764 cycles. So with four CPUs
contending, each critical section plus handover takes longer than a
whole uncontended entry did. The instrumented hold grows the same way, by
about 430 cycles from K=1 to K=4 (865 → 1 293), on the same code path. The
counts show no extra work under the lock at K=4 (slow paths stay under
0.02 %). That points to the kernel's shared data (the `joist_kernel`
record, the lock, thread and endpoint records) moving between the cores'
caches on every acquisition. That is an inference: no cache-miss counters
were read (no `perf` on the host, and none inside the guest).

What the hold is made of (instrumented, mean cycles per occurrence, K=4,
`analysis.txt` has all K):

| Inside the hold | passive (fast path) | own-context (slow path) |
|---|---|---|
| `abi_syscall` (the IPC operation itself) | 403 | 425 |
| address-space switch (CR3 write) | 330 | 333 |
| timer write (`apic_timer_arm`; leaves the VM) | rare (~900 in 2 s) | **6 383** (once per entry) |
| `joist_sched_pick_on` | slow path only | 398 |
| `joist_sched_next_event` | slow path only | 166 |
| `poke_others` (`joist_sched_better` per other CPU) | slow path only | 410 |
| the rest (entry, charging, register copies, instrumentation) | about 560 | about 780 |
| **Mean hold** | **1 293** | **8 891** |

## 3. Variants

### (a) A queue lock: MCS (`-DRUN_LOCK_MCS=1`)

One cache-line-aligned queue node per CPU; `xchg` on the tail; each waiter
spins on its own node and answers `cpu_sync` while it spins; a release
writes only the next waiter's node. Still first come, first served.

| | Ticket | MCS |
|---|---|---|
| Scaling, passive, K=1 / 2 / 3 / 4 (round trips/s) | 2 909 k / 2 115 k / 1 906 k / 1 657 k | 2 744 k / 2 008 k / 1 825 k / 1 604 k (8 boots) |
| ... against K=1 | 1 / 0.73 / 0.66 / 0.57 | 1 / 0.73 / 0.66 / 0.58 |
| Scaling, own-context, K=1 / 4 | 295 k / 251 k (0.85) | 295 k / 251 k (0.85) |
| Cycles per round trip, passive, K=1 / K=4 | 1 527 / 10 102 | 1 621 / 10 533 |
| ipcbench, uncontended (p50 cycles): null call / 2 spaces / 1 space | 380 / 1 448 / 988 | 432 / 1 504 / 1 056 |
| ipcbench, contended line, p50 (p99) | 8 768 (9 648) | 8 864 (9 712) |
| Stats build, passive K=4: waiting share, mean hold, mean wait | 75.6 %, 1 293, 4 933 | 75.2 %, 1 274, 4 820 |
| Stats build, ipcbench contended: waiting share, mean hold | 75.4 %, 1 347 | 75.1 %, 1 349 |

(ipcbench rows: 7 boots each, p50 medians over boots. The full lines with
ranges are in `analysis.txt`.)

**The MCS lock does not change the scaling** (within 1 to 3 %, inside the
run-to-run range). Uncontended it is dearer than the ticket lock: 52
cycles more on a null system call, 56 to 68 more on an IPC round trip (two
entries), 94 more per scaling-test round trip at K=1 (`xchg`, the
release's compare-and-swap, and `this_cpu()` in `unlock`). With at most 4 waiters, spinning on one shared
line rather than one each made no measurable difference; the hold time,
not the handover, is what the waiters wait for.

### (b) A shorter critical section

**The scans** (`joist_sched_pick_on`, `joist_sched_next_event`,
`poke_others`) run only on the slow path. Their measured cost per slow
acquisition (instrumented, so upper bounds):

| Workload | Scans per slow acquisition | Mean slow hold | Share |
|---|---|---|---|
| passive scaling, any K | (under 0.02 % of acquisitions are slow) | | about 0 |
| own-context scaling, K=1 / K=4 | 615 / 974 cycles | 8 000 / 8 891 | 8 % / 11 % |
| ipcbench contended (the three busy CPUs) | 592 cycles (pick 383, next 146, poke 62) | 1 347 | 44 % |

Estimate: removing the scans entirely would leave the passive scaling
test exactly as it is (it never scans), take about 1/10 off the
own-context hold, and nearly halve the busy CPUs' hold in ipcbench's
contended line. That is not measured, only computed from the timings.

**The timer write**, which the timings showed is most of the slow hold,
was measured as a variant, since it was one change: `-DRUN_LOCK_TIMER_OUT=1`
chooses the moment under the lock (as now) and writes this CPU's own
LAPIC timer after `unlock()`. Whether that is correct in general is not
established here (it touches only this CPU's LAPIC and `c->armed_for`,
which only this CPU uses). It ran clean in all 14 boots, plain and stats.

| Own-context, `ls-timerout`, 7 boots | K=1 | K=2 | K=3 | K=4 |
|---|---|---|---|---|
| Round trips/s, total | 296 k (294-300 k) | 514 k (505-524 k) | 738 k (734-761 k) | 899 k (892-922 k) |
| Against K=1 | 1.00 | **1.73** | **2.49** | **3.04** |
| Cycles per round trip (per pair) | 14 922 | 16 660 | 16 745 | 17 758 |
| Stats build: mean hold, cycles | 1 675 | 2 244 | 2 269 | 2 439 |
| Stats build: waiting share | 0.2 % | 2.2 % | 3.4 % | 20.0 % |

The plain kernel at the same points: 0.90, 0.88, 0.85, hold 8 000 to
8 891, waiting 75 % at K=4. Passive scaling is unchanged by this variant
(0.72, 0.64, 0.57: the fast path doesn't write the timer). The timer
write's own cost is the same in or out of the lock (6 300 cycles; it
leaves the VM, which costs the same either way). Only who waits for it
changes.

Not tried: moving the CR3 write (about 330 cycles a switch, also
CPU-local in itself) out of the lock. The TLB shootdown (`others_leave`)
reads other CPUs' `cur_space` under the lock, so it would need more than
a measurement. Also not tried: shortening the fast path's hold, whose
largest parts are `abi_syscall` itself and the CR3 write.

## Pause-loop exits (KVM's PLE)

PLE is enabled on the host (`kvm_intel` `ple_gap` 128, `ple_window`
4096). There is no root and no debugfs here, and no `perf`. But QEMU's
QMP `query-stats` (target `vcpu`) reads KVM's per-vCPU statistics without
either. One boot each of `ls-ticket`, `ls-mcs`, `lb-ticket` and `ls-1cpu`
was made with a QMP socket read once a second (`tools/qmpstats.py`;
`raw/ple-*.kvmstats`, `raw/ple-*.log`).

- `directed_yield_attempted` and `directed_yield_successful` stayed **0
  on every vCPU in every boot**. KVM counts these when a pause-loop exit
  tries to yield to another vCPU. They aren't a count of the exits
  themselves (KVM keeps none), so 0 doesn't prove no pause-loop exits
  happened.
- A bound from the exit counts: in the passive phases, with 4 vCPUs
  spinning on the lock 75 % of the time, each vCPU made 1 100 to 1 300
  exits a second. About 1 000 of them were host-interrupt exits
  (`irq_exits`), the rest the guest's 10 ms timer. **So at most about 100
  to 300 exits a second per vCPU could be pause-loop exits**, against
  millions of lock acquisitions a second: not a measurable cost here. In
  the own-context phases exits are 2 per round trip, matching the timer
  writes, leaving no room for a significant number of others either.

## Found on the way

- **benchroot starts its passive servers in a way that can hang on more
  than one CPU.** It binds a context to the server and unbinds it at once,
  without the start-up call `docs/abi.md` requires on SMP ("its starter
  ... calls the server once and, when the answer is back, unbinds"). The
  first boot of `lb-ticket-stats` here stopped with "no thread can ever
  run again" after its first measurement. The second measurement's server
  never reached its `Recv`. `patch.diff` adds the call to benchroot (the
  lb images measured above all have it). scaleroot does the same.
- On the stats kernels ipcbench's null system call's median reads 732
  to 792 cycles against 380 to 440 on the plain ones: the instrumentation (about
  five timed TSC reads per entry) roughly doubles a call that small. Only
  the plain kernels' ipcbench figures (section 3a) are comparable with
  `docs/phase3-data`. With and without the stats, the ruler's greatest
  reading during the null-call measurement is about 39 000 counts in 26
  of the 28 ipcbench boots, against
  about 7 000 otherwise: the effect `docs/phase3-data/README.md` records
  for the bench gate, seen here too and not looked into.

## What could not be measured

- Bare metal: everything is first-level KVM. The timer write in
  particular (about 6 300 cycles, a VM exit) is a KVM cost. On bare metal,
  or with a TSC-deadline timer, the own-context figures and the
  timer-outside variant's effect would be different. I can't say by how
  much.
- Cache-miss and coherence-traffic counts: no `perf` on the host, no PMU
  use in the guest. Cache-line migration as the cause of longer holds
  under contention is inferred, not measured.
- Pause-loop exits directly: KVM has no counter for them. The bound
  above is what QMP's statistics allow.
- More than 4 CPUs: the kernel's `RUN_MAX_CPUS` and `JOIST_MAX_CPUS` are
  4, and the P53 has 6 cores.
- The 1-CPU own-context reference (a lower bound, as noted).

## Files

- `patch.diff`: from `~/joist` to `~/joist-lock` (`diff -ruN joist
  joist-lock -x build -x '*.o'`); `git apply --check -R -p1` passes in
  `~/joist-lock`. It contains `run.c` (`RUN_LOCK_STATS`, `RUN_LOCK_MCS`,
  `RUN_LOCK_TIMER_OUT`; all off by default, and the default kernel's code
  is unchanged), `user/scaleroot/` (new), ipcbench's role 3 and
  `BENCH_LOCK_STATS`, benchroot's start-up call, `user/Makefile`.
- `raw/`: every kept serial log (`VARIANT-N.log`; numbers have gaps, an
  artefact of the run script), `ple-*` (logs and KVM statistics),
  `images.sha256`.
- `analysis.txt`: `tools/parse.py` over `raw/`, all figures above and
  more (every K, every timed item).
- `tools/`: `runiso.sh` (one boot), `campaign.sh` (the rounds, with the
  idle check), `qmpstats.py`, `parse.py`.
- Not kept here: boots discarded for a busy build VM, the round of
  stats and lb boots taken before the timer and CR3 timings (stats) and
  before the benchroot fix (lb) were added. They are in
  `/srv/joist/test/lockstudy/` (`discarded-*`, `old-stats/`, `old-lb/`).
