# Phase 3 data

Raw evidence behind the numbers in `docs/phase3-plan.md` *Progress*, as for
Phases 1 and 2. Environments are in `docs/testbed.md`. Until 2026-10-14,
`joist-dev` (VM 113 on pve1) built and ran every host test and the QEMU (TCG)
boots, and VM 112 on pvehome booted images on a real Intel CPU through KVM;
the files below from before that date name those machines. Since then the
build VM is `joist-dev2` on the P53, and images boot on the P53's own CPU
through KVM (`tools/p53/boot.sh`).

## Step 4 (2026-10-10): `step4/`

The step's goal on VM 112: the root task loads `pong` and `ping` from boot
modules and they make 200 000 round trips across address spaces. Image:
`kernel/build/selftest-pingpong-long/joist.iso` built on joist-dev by
`sh test/selftest.sh` (`ROOT_TASK=root MODULES="pong ping"
UEXTRA=-DPING_ROUNDS=200000u`) at the commit that adds these files, SHA-256
`76d621b345bc1b748847a9e9fc22b601a42907890236eee8a3a5442395ae17fc`; booted with
`tools/pve/boot.sh ISO bios|uefi 120 LOG`.

| Firmware | Serial log (from `joist: boot`) | Result |
|---|---|---|
| SeaBIOS | `step4/vm112-pingpong-long-bios.txt` | status 1 (the root task exited 0); 200 000 round trips; 33 timer ticks at exit, 20 of them before the root task starts |
| OVMF | `step4/vm112-pingpong-long-uefi.txt` | the same; 35 ticks |

So about 13 to 15 ticks (130 to 150 ms) for 200 000 round trips: roughly
0.65 to 0.75 µs each under KVM on the i5-14400. This is the 100 Hz tick
counter, not a benchmark (step 8 measures with the TSC); it is recorded as
the first evidence that IPC on real hardware is in the expected range.

## Step 5 (2026-10-10): `step5/`

The scheduler on VM 112, at the commit that adds these files. Images built
on joist-dev by `make check` in `kernel/` (`sh test/selftest.sh`), booted
with `tools/pve/boot.sh ISO bios|uefi 120 LOG`:

| Image | Built as | SHA-256 |
|---|---|---|
| `kernel/build/selftest-sched/joist.iso` | `ROOT_TASK=schedroot MODULES=worker` | `b9598017a05745223c5c73813ef8d64f2124f436447ad0e3364b72175bb57fd1` |
| `kernel/build/selftest-pingpong-long/joist.iso` | `ROOT_TASK=root MODULES="pong ping" UEXTRA=-DPING_ROUNDS=200000u` | `63b53c9fa3329c148a603e3ac30eb7f195a00abf8decf7fc1bcb79d1f8c93786` |

| Run | Serial log (from `joist: boot`) | Result |
|---|---|---|
| Scheduler test, SeaBIOS | `step5/vm112-sched-bios.txt` | status 1 (exited 0). Budget of 2 ms in 10: 20 %. Spinner beside system calls on that budget keeps 81 %. Equals: 50 %. Passive server on its client's budget: 19 % (1 193 calls); the spinner beside keeps 80 % |
| Scheduler test, OVMF | `step5/vm112-sched-uefi.txt` | status 1. 19 %, 79 %, 50 %, 19 % (1 196 calls), 80 % |
| Ping-pong, 200 000 round trips, SeaBIOS | `step5/vm112-pingpong-long-bios.txt` | status 1; 9 timer interrupts after boot |
| Ping-pong, OVMF | `step5/vm112-pingpong-long-uefi.txt` | status 1; 13 |

Each line of the scheduler test prints the two counts behind its
percentage. In the ping-pong run a timer interrupt now marks the end of a
10 ms budget (the timer is one-shot), so 9 to 13 of them is roughly 90 to
130 ms for 200 000 round trips: 0.45 to 0.65 µs each, where step 4 measured
0.65 to 0.75 µs by the same rough method. Still not a benchmark (step 8).

Two earlier boots of this step are described in `docs/phase3-plan.md`
(*Step 5*, parts 4 and 5) and not kept as files: the kernel before kernel
time was charged, which fails the scheduler test on this VM; and the kernel
that wrote the timer on every exit, on which the same ping-pong took 51
interrupts (about 2.5 µs a round trip).

## Step 6 (2026-10-10): `step6/`

The fault test on VM 112, at the commit that adds these files. Image
`kernel/build/selftest-fault/joist.iso` (`ROOT_TASK=faultroot
MODULES=faulter`), built on joist-dev by `make check` in `kernel/`, SHA-256
`285d678eb3bde5fc01f9a669ef617da24dd8288d927ce1dbce922c1b347e86e4`; booted
with `tools/pve/boot.sh ISO bios|uefi 120 LOG`.

| Firmware | Serial log (from `joist: boot`) | Result |
|---|---|---|
| SeaBIOS | `step6/vm112-fault-bios.txt` | status 1 (the root task exited 0): all six tests; both tasks with no usable handler are reported destroyed |
| OVMF | `step6/vm112-fault-uefi.txt` | the same |

## Step 7 (2026-10-13): `step7/`

The destruction test on VM 112, at the commit that adds these files. Two
images built on joist-dev2 (VM 114) by `make selftest` in `kernel/`, both
with `ROOT_TASK=cascaderoot MODULES=worker` and the larger tables
`KSIZES="-DJOIST_MAX_OBJECTS=8192u -DJOIST_MAX_MAPPINGS=512u
-DJOIST_MAX_CSPACES=64u -DROOT_BUDGET=16384u"`; booted with
`tools/pve/boot.sh ISO bios|uefi 120 LOG`.

| Image | SHA-256 |
|---|---|
| `kernel/build/selftest-cascade/joist.iso` | `c4167a51e8ee0ad5ba6b11b398edf0ebfc180d49304465cae1205cf384248212` |
| `kernel/build/selftest-cascade-nopreempt/joist.iso` (`KEXTRA=-DRUN_PREEMPT_SLOTS=0u`) | `ffd369140f544b1b8b8852995ed3fc44f78d5aea21ed9494c63677e3e0face12` |

| Run | Serial log (from `joist: boot`) | Result |
|---|---|---|
| The kernel, SeaBIOS (the last of three runs) | `step7/vm112-cascade-bios.txt` | status 1: a cascade of 4 848 capabilities takes 976 us alone; a ticker every 97 us never waits more than 126 us during 20 of them (115 us with nothing else running); 62 tasks can be created before and after |
| The kernel, OVMF | `step7/vm112-cascade-uefi.txt` | status 1: 670 us; ticker every 67 us, longest wait 90 us (78 us) |
| A kernel that never preempts, SeaBIOS | `step7/vm112-cascade-nopreempt-bios.txt` | status 7, as required: the ticker woke once per cascade and waited 1 326 us, a whole one |

The VM's speed varied between runs by up to a factor of two (the build VM
shares the host, `docs/testbed.md`); the test measures everything against
the cascade's own length in the same run.

## Step 8 (2026-10-13): `step8/`

The IPC benchmark on VM 112 (KVM, Intel Core i5-14400, one virtual CPU on a
P-core of its own), at the commit that adds these files; joist-dev2 idle.
Two images built on joist-dev2 by
`make BUILD=build/benchN USER_BUILD=build/bench UEXTRA=-DBENCH_CALLS=100000u
KEXTRA=-DRUN_FAST_PATH=N ROOT_TASK=benchroot MODULES=ipcbench
build/benchN/joist.iso` in `kernel/`; each booted ten times with
`tools/pve/boot.sh ISO bios 180 LOG`.

| Image | SHA-256 |
|---|---|
| fast path (`RUN_FAST_PATH=1`, the default kernel) | `5315a51cac92c30b6b361bc56ef0227802cafdc47cb5d9fe7c2d4faeca8c39fd` |
| slow path only (`RUN_FAST_PATH=0`: the scheduler asked every time, `iretq` always) | `47a935560c817766fdcf44270b5753ece241d9fb7ad44a6778f81f93eaedbf05` |

Files: `step8/vm112-ipcbench-fast-10runs.txt` and
`step8/vm112-ipcbench-slow-10runs.txt` (the benchmark's lines of each run),
`step8/vm112-ipcbench-fast-bios.txt` (one whole serial log).

Each figure is a round trip (`Call` to its return) in **CPU cycles** by the
benchmark's ruler, the clock reading's own cost taken off; 20 segments of
100 000 calls; the range is over the ten runs.

| Measurement | Fast path: median | 99th percentile | Slow path: median | 99th percentile |
|---|---|---|---|---|
| A system call that does nothing | 372-376 | 416-428 | 580-584 | 628-632 |
| Two address spaces, passive server | 1 356 (all ten) | 1 480-1 508 | 1 896 (all ten) | 2 032-2 068 |
| One address space, passive server | 764 (all ten) | 852-860 | 1 292-1 296 | 1 404-1 424 |
| Two address spaces, server with its own context | 9 732-9 764 | 9 932-10 908 | 9 992-10 020 | 10 128-11 316 |

Against the plan's meaning of *stable* (across 10 runs the median moves
less than 5 % and the 99th percentile less than 20 %): medians move 1.1 %
at most, 99th percentiles 9.8 % (fast, last row) and 11.7 % (slow, last
row), under 2 % for the three passive measurements.

At about 4.1 GHz (the turbo step most runs sat at) the fast figures are
0.33 us and 0.19 us a round trip. The last row is as slow as it is because
every switch between two scheduling contexts sets the timer, and a timer
write leaves the virtual machine; it is not what the fast path is for, and
on bare metal it would not cost this.

Not bare metal: this is KVM on a shared host (`docs/testbed.md` lists what
that did to the measurements and what the benchmark does about it). The
unit and method are in `user/ipcbench/ipcbench.c`.

## Step 9 (2026-10-13): `step9/`

More than one CPU, on VM 112's four (KVM, four P-cores of the i5-14400, one
virtual CPU pinned to each), at the commit that adds these files. Images
built on joist-dev2 by `make check` in `kernel/` (the default kernel, which
uses up to 4 CPUs); booted with `tools/pve/boot.sh ISO bios|uefi 180 LOG`.

| Run | Serial log | Result |
|---|---|---|
| The multiprocessor test (`smproot`), SeaBIOS | `step9/vm112-4cpus-smp-bios.txt` | status 1: 4 spinners together do 357 % of what one does alone; priorities hold across CPUs; 99 629 calls answered between two CPUs in 0.3 s; a thread on another CPU stopped, read and resumed 50 times, and killed; 20 rounds of tasks destroyed while their threads ran |
| The same, OVMF | `step9/vm112-4cpus-smp-uefi.txt` | status 1 |
| Fault test | `step9/vm112-4cpus-fault-bios.txt` | status 1 (it includes a page unmapped from a task running on another CPU) |
| Destruction test | `step9/vm112-4cpus-cascade-bios.txt` | status 1 |
| Scheduler test | `step9/vm112-4cpus-sched-bios.txt` | status 1 (it keeps three CPUs busy and runs on the fourth) |
| Ping-pong, 200 000 round trips | `step9/vm112-4cpus-pingpong-long-bios.txt` | status 1 |

**The kernel lock's cost** (decision 0016 asks for it on 1, 2 and 4 CPUs):
`step9/vm112-ipcbench-1-2-4-cpus.txt`, the benchmark of step 8 on kernels
built with `KEXTRA=-DRUN_MAX_CPUS=1u`, `2u` and `4u`
(`UEXTRA=-DBENCH_CALLS=100000u`), three runs each. Medians, CPU cycles per
round trip:

| | 1 CPU (no lock taken) | 2 CPUs | 4 CPUs |
|---|---|---|---|
| A system call that does nothing | 384 | 480 | 480 |
| Two address spaces, passive server | 1 356 | 1 444 | 1 444 |
| One address space, passive server | 772 | 864 | 860 |
| Two address spaces, server with its own context | about 9 900 | about 9 990 | about 10 190 |
| Two address spaces, passive server, **every other CPU making system calls as fast as it can** | | 4 220 to 4 440 | 9 010 to 9 600 |

So: taking and releasing the lock with nobody else wanting it costs about
45 cycles a kernel entry (6 % of a round trip between address spaces), the
same on 2 and 4 CPUs. Fought over by CPUs that do nothing but enter the
kernel, a round trip takes 3 times as long on 2 CPUs and 6.5 times on 4:
the lock is fair, so each entry waits its turn behind every other CPU's,
and the kernel's data moves between their caches each time. Under KVM a
CPU waiting for a lock is also dearer than on bare metal. This is the worst
case for one lock, measured as 0016 asked, for the decision to be looked
at again with numbers.

(Image SHA-256, in order: `fa87235c3d647b9b26b19f4f1082f7b7cabb13290ea569b4f907c3aa4f2293c9`,
`f6f8fc892223a337f7c45e444eafc9a85a61470845c934229f611140da2e8aaa`,
`b107fb0496e8c27d9a6740017557a6c2fd659e25f961dc3c0c5b75e7b1ae1623`.)

## The benchmark gate's baseline moves to the P53 (2026-10-15)

`kernel/test/ipcbench.baseline` was measured on the Intel Core i5-14400
(Phase 3 step 8). After the move to the P53 (`docs/testbed.md`) the gate
runs on an Intel Core i7-9850H, under KVM inside the build VM, and the first
full run there gave 452, 1 336 and 880 cycles: the first within 5 points of
the 25 % limit, from the change of CPU rather than of code. Measured again
there (a hand-off to the P53's Claude Code; its report is kept on the P53 in
`/srv/joist/handoff/smallbuild/findings.md`): 20 runs of the `bench-gate`
entry, nothing else running on the build VM.

| | Old baseline (i5-14400) | 20 runs on the P53 | New baseline |
|---|---|---|---|
| A system call that does nothing | 376 | 336-352 in 16 runs; 444-476 in 4 | 384 |
| Two address spaces, passive server | 1 356 | 1 336-1 344 | 1 344 (their median) |
| One address space, passive server | 764 | 876-880 | 880 (their median) |

The system call's figure falls in two groups, about 30 % apart. In the
low group the benchmark's ruler (the chain of multiplications that converts
counts to CPU cycles) itself reads up to 39 000 counts against about 7 000
otherwise, so a disturbed ruler is the first suspect; the cause is not
established. Its baseline is the lowest whose limit (480) passes both
groups, not the median, and the margin is thin (476 against 480): an open
item, for the exit step's benchmarks.

The same hand-off found that the fourth measurement (a server with its own
scheduling context) had always printed the histogram's top edge, 16 388,
as its median once it was slower than 16 384 cycles, as it is under KVM in
the build VM. The histogram's buckets are now sized from the warm-up; the
measurement reads about 93 000 cycles there (each change of context sets
the timer, which under nested KVM is very dear).


## Step 10: the two groups explained, and the run queues' cost (2026-10-15): `step10-runq/`

**The two groups** of the do-nothing system call (above) had one cause,
found in 20 logs of an earlier hand-off: `ipcbench` measured the cost of
reading the clock once, at the start, while the CPU was still slow from
idle (100 counts against 16-26 later), and took that off every sample. In
the runs where that happened, every figure came out too low; the high
group, 444-476, is the true one. `ipcbench` now measures the clock's cost
with every batch, beside the ruler, and prints its range ("clock 16 to 26
counts, taken off"). The baselines above, the system call's 384 in
particular, were set with the old method and are measured again with the
exit benchmark.

**The run queues' cost.** Every image below was booted on the P53's own
CPU through KVM (`tools/p53/boot.sh ISO bios 120 LOG` with `P53_SMP=1`, as
the gate's `bench-gate` entry; QEMU 11.1.2, i7-9850H), alternating between
images so that the machine's state is shared, from the workstation while
the build VM was otherwise quiet. Images are the `bench-gate` entry
(`ROOT_TASK=benchroot MODULES=ipcbench`), built on joist-dev2;
`step10-runq/images.tsv` gives each one's SHA-256 and what it is, and
`medians.tsv` each run's medians (`p50`, cycles). `<run>.txt` holds that
run's `ipcbench` lines.

| Image | Null call | Round trip, two spaces | One space | 4 KiB each way |
|---|---|---|---|---|
| A: the scan (`93fb28d`, fixed `ipcbench`) | 472 | 1 376 | 920 | 2 736 |
| B: run queues, first version | 480 | 1 728 | 1 272 | 3 088 |
| H: run queues, final version | 496 | 1 548 | 1 096 | 2 904 |

The first version cost 352 cycles a round trip (+26 %). The scan cost the
fast path nothing (it scanned only when the slow path chose), while queues
are kept up to date at every change of state, and a Call moved the server
onto a queue and off it again twice. What brought it down (C to H in
`images.tsv`): a thread already in its place is left alone; the kernel
holds the queues for a whole system call and files each changed thread
once after the direct switch, so a thread handed the CPU never goes on a
queue at all; the common case is inline. Ruled out on the way, each by a
measurement: the larger records and their layout (X3: no change), the
JCC-erratum alignment of jumps (Aj, Ej: the gap stays), the system-call
layer (X6), and a fast path no longer taken (As, Ds: the gap is the same in
the slow path). TSC probes (Pab, Prq) put most of what is left inside the
core's operations.

**What is left: 172 cycles a round trip (+12.5 %), 24 on a do-nothing
call.** The price of choosing a thread in a time that doesn't depend on
the number of threads (choice 9), which the project owner asked for on
2026-10-15. With today's 32 threads the scan was cheap; the queues' gain
is in the bound, not in today's speed.

## Step 10: the kernel lock measured for decision 0017 (2026-10-15): `step10-lock/`

The lock study asked for when decision 0016 was to be revisited, run on the
P53 by its Claude Code: `findings.md` is its report (set-up, method, every
figure with its range, what could not be measured); `analysis.txt` is
`tools/parse.py` over `raw/`; `raw/` holds every kept serial log, the KVM
statistics of the pause-loop runs and `images.sha256`; `tools/` the scripts
that ran it (`runiso.sh` one boot, `campaign.sh` the rounds with the
build-VM idle check, `qmpstats.py`, `parse.py`). The kernel and user changes
it measured with (`RUN_LOCK_STATS`, `RUN_LOCK_MCS`, `RUN_LOCK_TIMER_OUT`,
all off by default; `user/scaleroot`; ipcbench's role 3; benchroot's
start-up handshake) are in the tree. The summary and what it decides are
in `docs/decisions/0017-kernel-lock-revisited.md`.

## Step 10: the exit benchmark (2026-10-15): `step10-exit/`, `step10-gate/`

**The exit benchmark.** The `bench-gate` entry's image (`ROOT_TASK=benchroot
MODULES=ipcbench`), built on joist-dev2 from the final step 10 tree with the
fast path (`fast`) and without it (`slow`, `-DRUN_FAST_PATH=0`); SHA-256s in
`step10-exit/images.sha256`. Booted 10 times each on the P53's own CPU
through KVM, alternating, with `tools/p53/boot.sh ISO bios 120 LOG` (4 vCPUs
pinned to host CPUs 2-5, QEMU 11.1.2, SeaBIOS), from the workstation, the
build VM idle. Each run's `ipcbench` lines are `step10-exit/<kernel>-<n>.txt`;
`summary.tsv` gives, per measurement, the median of the runs' medians, their
range, and the range of their 99th percentiles, each as a share of its median.

The plan's test (*Testing*): across 10 runs the median moves less than 5 %
and the 99th percentile less than 20 %.

| Measurement (cycles) | Fast: median (spread) | p99 spread | Slow: median (spread) | p99 spread |
|---|---|---|---|---|
| A system call that does nothing | 528 (0.8 %) | **29.4 %** | 836 (0.5 %) | 17.3 % |
| Two address spaces, passive server | 1 624 (0.2 %) | 3.6 % | 3 168 (0.8 %) | 8.8 % |
| One address space, passive server | 1 164 (0.0 %) | 6.4 % | 2 712 (0.9 %) | 2.6 % |
| Two address spaces, server with its own context | 13 808 (2.1 %) | 15.1 % | 14 640 (1.7 %) | 16.0 % |
| 4 KiB of IPC buffer each way | 2 976 (0.3 %) | 6.6 % | 4 532 (2.3 %) | 3.5 % |
| Every other CPU making system calls | 9 072 (**8.5 %**) | 9.7 % | 10 768 (**7.9 %**) | 8.0 % |

**Every uncontended round trip passes**, fast and slow. Two figures do not:
the do-nothing call's 99th percentile on the fast path (two runs of ten read
736 and 740 against 572 in the others), and the contended line's median,
whose spread is the lock's (decision 0017's measurements show 75 % of CPU
time waiting at 4 busy CPUs). These boots ran with 4 vCPUs, as `boot.sh`
does by default; the gate runs with one, inside the build VM (below), which
is why its figures are lower.

**The gate's baseline** (`kernel/test/ipcbench.baseline`): 10 runs of the
`bench-gate` entry inside the build VM, nothing else running there, on the
same tree (image SHA-256
`240764ba9bbd47639e5ea8eb40f1a6031f9449fb2247f6190c48463b920134f4`, QEMU
10.0.13); each run's lines in `step10-gate/`. The do-nothing call read 492 to
496 in all 10 runs, the round trips 1 548 and 1 088 to 1 092: the two groups
seen on 2026-10-14 are gone with the clock-cost fix. New baseline 496, 1 548,
1 092, from 384, 1 344, 880; why each moved is recorded in the file.

## Step 10: fault injection (2026-10-15): `step10-faultinj/`

`findings.md` is the report of the hand-off that built `user/faultinj` and
`user/fiworker` (run by the P53's Claude Code; summarised in
`docs/phase3-plan.md`, *Step 10, part 3*): the scenarios and kill points,
the seven self-tests (seeds 1-7, 100 rounds each), the 9 000 further rounds
(seeds 11-16, 300 rounds, five machine shapes), the six bugs planted in the
core by hand and the round that caught each, and what is not covered. The
self-tests themselves run in every `make check` in `kernel/`.

## Decision 0017's timer order, measured (2026-10-15): `step10-timerout/`

The timer written after the lock's release (the new default) against the
old order, by the P53's Claude Code with the lock study's method and tools:
`findings.md` is its report (scaling with K = 1 to 4 pairs, uncontended
`ipcbench`, 50 stress boots, image SHA-256s); `raw/` every kept log and
`stress.txt`; `tools/` the scripts. Summary in `docs/phase3-plan.md`,
*Decision 0017: the timer written after the unlock*.
