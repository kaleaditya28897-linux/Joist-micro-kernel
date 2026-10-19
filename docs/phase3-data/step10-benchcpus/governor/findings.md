# Bench fix: IPC benchmark under the `performance` governor (host CPUs 2-5)

Answer: **yes for the first measurement.** In 20 boots with CPUs 2-5 on `performance`, "a system call that does nothing" had p50 528-532 and p99 576 in every run (p99 spread 0.0 %, p50 0.8 %). No run had a slow first measurement: the ruler stayed at 6 770-7 420 counts per 12 000 cycles in every run, against 9 736-39 156 in powersave runs 1 and 8. Under powersave every run's null-call ruler also reached ~39 100 counts at some point; under performance it never went above 7 420.

**One new problem: another line fails.** "two address spaces, server with its own context" has a p99 spread of 25.4 % over the 20 runs (19 424-24 448; run 7 = 24 448, run 8 = 22 208, run 17 = 21 984). That is 25.2 % in runs 1-10 and 13.0 % in runs 11-20. Under powersave it was 15.1 %. So on runs 1-10 alone the plan's criterion (p50 < 5 %, p99 < 20 % across 10 runs) fails on this line, and on runs 11-20 alone it passes. Every other line passes in both halves and over all 20 runs. Its p50 is stable (2.7 %), and its ruler is normal in those runs (A ≥ 6 770, B ≤ 7 708), so this is not CPU frequency. It looks like ordinary tail noise of the context-switching path, which powersave's 10 runs were too few to show. This needs a separate look; I measured only.

## Method

- Image: `iso/fast.iso`, SHA-256 e9c79ea708187202f153af3517e0c6741dd35ae5402152bb1b1587161fec9d5c (checked).
- Booted 20 times with `/srv/joist/handoff/lockstudy/tools/runiso.sh` (unchanged): q35/KVM, SeaBIOS, `-smp 4`, vCPU n pinned to host CPU 2+n (pins recorded in each log), under `flock /srv/joist/test/lock`, timeout 400 s. Driver: `tools/campaign.sh` (based on lockstudy's campaign.sh).
- Build VM idle gate: the build VM's /proc/stat busy share (via ssh -p 2222 aditya@127.0.0.1) over 5 s had to be < 2 % before a boot. A boot was kept only if the share over the boot was also < 2 %. All 20 boots passed on the first try (0.2-0.4 %), so none were discarded. All 20 ended with `joist: finish code 0`.
- Governor: `scaling_governor` of CPUs 2-5 was checked before every boot (the script stops if it is not `performance`) and after every boot. It was `performance performance performance performance` at the start of the work, before and after each of the 20 boots (each log's first line, `raw/campaign.log`), and at the end. CPUs 0, 1, 6 and 7 were on `powersave` (not changed by me). Driver: intel_pstate.
- Frequency: while each boot ran, `scaling_cur_freq` of CPUs 2-5 was sampled every 0.2 s into `raw/fast-N.freq` (epoch time, QEMU-running flag, four kHz values). Only samples taken while the boot's QEMU process existed are summarised.
- Analysis: `tools/analyze.py` (output `raw/tables.md`). Spread = (max-min)/median of the runs' values. Powersave figures come from the 10 given `powersave-runs/fast-N.txt` files, recomputed with the same script; they match `summary.tsv`.
- Timing: 2026-10-15 14:09-14:12 UTC. All boots ran back to back (~8 s each including the idle check; QEMU ran ~2 s).

## Tables

### performance governor (20 runs)

| measurement | p50 median | p50 min-max | p50 spread | p99 median | p99 min-max | p99 spread | ruler A min-max | ruler B min-max |
|---|---|---|---|---|---|---|---|---|
| a system call that does nothing | 528 | 528-532 | 0.8 % | 576 | 576-576 | 0.0 % | 6770-7244 | 6934-7420 |
| two address spaces, passive server | 1626 | 1624-1628 | 0.2 % | 1824 | 1708-2016 | 16.9 % | 6770-7244 | 7436-7840 |
| one address space, passive server | 1172 | 1164-1172 | 0.7 % | 1204 | 1180-1308 | 10.6 % | 6770-7416 | 7264-7624 |
| two address spaces, server with its own context | 14048 | 13856-14240 | 2.7 % | 19744 | 19424-24448 | 25.4 % | 6774-7080 | 7278-7708 |
| two address spaces, passive server, 4 KiB of IPC buffer each way | 2976 | 2976-2984 | 0.3 % | 3096 | 2992-3272 | 9.0 % | 6920-7416 | 7272-7726 |
| two address spaces, passive server, every other CPU making system calls | 9184 | 8960-9376 | 4.5 % | 10064 | 9856-10784 | 9.2 % | 7242-7416 | 7610-7654 |

### powersave governor (given) (10 runs)

| measurement | p50 median | p50 min-max | p50 spread | p99 median | p99 min-max | p99 spread | ruler A min-max | ruler B min-max |
|---|---|---|---|---|---|---|---|---|
| a system call that does nothing | 528 | 528-532 | 0.8 % | 572 | 572-740 | 29.4 % | 6770-9738 | 39100-39156 |
| two address spaces, passive server | 1624 | 1620-1624 | 0.2 % | 1662 | 1636-1696 | 3.6 % | 6770-7242 | 6946-8692 |
| one address space, passive server | 1164 | 1164-1164 | 0.0 % | 1196 | 1172-1248 | 6.4 % | 6770-6922 | 6940-7436 |
| two address spaces, server with its own context | 13808 | 13760-14048 | 2.1 % | 19760 | 19392-22368 | 15.1 % | 6770-6922 | 7086-7618 |
| two address spaces, passive server, 4 KiB of IPC buffer each way | 2976 | 2968-2976 | 0.3 % | 3040 | 3000-3200 | 6.6 % | 6770-7078 | 6954-7616 |
| two address spaces, passive server, every other CPU making system calls | 9072 | 8768-9536 | 8.5 % | 9872 | 9504-10464 | 9.7 % | 7242-7244 | 7438-7622 |

### Performance runs split into two 10-run halves

- runs 1-10, null call: p50 spread 0.8 %, p99 spread 0.0 %
- runs 1-10, contended: p50 spread 3.5 %, p99 spread 9.2 %
- runs 11-20, null call: p50 spread 0.8 %, p99 spread 0.0 %
- runs 11-20, contended: p50 spread 4.2 %, p99 spread 6.7 %

### Per run, performance

| run | null p50 | null p99 | null ruler A-B | contended p50 | contended p99 |
|---|---|---|---|---|---|
| 1 | 532 | 576 | 6922-7282 | 9152 | 9920 |
| 2 | 528 | 576 | 6772-7296 | 9184 | 10368 |
| 3 | 532 | 576 | 7244-7292 | 9216 | 10080 |
| 4 | 532 | 576 | 6770-7118 | 9184 | 10080 |
| 5 | 532 | 576 | 7244-7276 | 9120 | 10144 |
| 6 | 528 | 576 | 6772-7418 | 9280 | 10784 |
| 7 | 528 | 576 | 6770-7098 | 9184 | 9984 |
| 8 | 528 | 576 | 6772-6934 | 9216 | 10208 |
| 9 | 528 | 576 | 6772-7102 | 8960 | 9856 |
| 10 | 528 | 576 | 6772-7098 | 9216 | 9920 |
| 11 | 528 | 576 | 6772-7418 | 9280 | 10560 |
| 12 | 528 | 576 | 6770-7096 | 9120 | 9888 |
| 13 | 532 | 576 | 7080-7112 | 8992 | 10048 |
| 14 | 532 | 576 | 7078-7280 | 9120 | 9920 |
| 15 | 528 | 576 | 6772-6952 | 9088 | 9952 |
| 16 | 532 | 576 | 7244-7420 | 9120 | 10176 |
| 17 | 528 | 576 | 6772-7104 | 9216 | 9952 |
| 18 | 528 | 576 | 6770-7276 | 9120 | 10528 |
| 19 | 528 | 576 | 6772-7278 | 9216 | 9888 |
| 20 | 528 | 576 | 6770-6950 | 9376 | 10400 |

### Per run, powersave

| run | null p50 | null p99 | null ruler A-B | contended p50 | contended p99 |
|---|---|---|---|---|---|
| 1 | 528 | 740 | 9738-39116 | 8768 | 9504 |
| 2 | 528 | 572 | 6772-39124 | 8896 | 9632 |
| 3 | 532 | 576 | 6772-39156 | 9088 | 9824 |
| 4 | 528 | 572 | 6772-39112 | 9056 | 9824 |
| 5 | 532 | 576 | 7244-39132 | 9152 | 10368 |
| 6 | 528 | 572 | 6772-39148 | 9536 | 10464 |
| 7 | 528 | 572 | 6772-39100 | 9104 | 10048 |
| 8 | 528 | 736 | 9736-39132 | 8960 | 9792 |
| 9 | 528 | 572 | 6770-39116 | 9248 | 10208 |
| 10 | 528 | 572 | 6772-39144 | 8896 | 9920 |

### Slow first measurement (null call ruler A > 8000 counts)

- performance: none
- powersave: [1, 8]

### Frequency samples (kHz, only while the boot's QEMU ran)

| run | samples | CPU2 min/med/max | CPU3 min/med/max | CPU4 min/med/max | CPU5 min/med/max |
|---|---|---|---|---|---|
| 1 | 10 | 4100522/4264795/4405712 | 4099966/4268124/4472406 | 800000/2449989/4425012 | 800000/2449999/4350000 |
| 2 | 10 | 800000/4222659/4364459 | 4099986/4301351/4524638 | 800000/800000/4294235 | 800000/800000/4408877 |
| 3 | 10 | 4100615/4331940/4445568 | 800000/4168715/4351338 | 800000/4308000/4411827 | 800000/800000/4264376 |
| 4 | 10 | 800000/4237235/4359375 | 800000/4234623/4466643 | 800000/800000/4253204 | 800000/4099989/4400576 |
| 5 | 10 | 800000/4309188/4514829 | 800000/2484953/4405793 | 800000/4248465/4309102 | 800000/4170471/4309480 |
| 6 | 10 | 800000/4217554/4416447 | 800000/4290301/4443645 | 800000/4144890/4330778 | 800000/4143492/4358994 |
| 7 | 10 | 4096643/4281514/4370850 | 800000/4173796/4398382 | 800000/4120577/4447445 | 800000/4120489/4328561 |
| 8 | 10 | 800000/4265872/4454364 | 800000/800000/4327569 | 800000/2450001/4329153 | 800000/4110284/4328756 |
| 9 | 10 | 800000/4236046/4476533 | 800000/4114911/4346291 | 800000/4318523/4433779 | 800000/4221058/4400628 |
| 10 | 10 | 800000/4240533/4413207 | 4099975/4187365/4413147 | 800000/4135705/4340975 | 800000/2449996/4469930 |
| 11 | 10 | 800000/4232536/4405191 | 800000/4306737/4528666 | 800000/800000/4354954 | 800000/4105953/4291159 |
| 12 | 10 | 4100030/4289799/4531688 | 800000/4192194/4342711 | 800000/4140547/4339933 | 800000/4140543/4403669 |
| 13 | 10 | 800000/4258048/4405071 | 800000/4195499/4416152 | 800000/4181114/4296239 | 800000/4257586/4407097 |
| 14 | 10 | 4098053/4266005/4369148 | 800000/800000/4379140 | 800000/4122437/4339142 | 800000/4172129/4503177 |
| 15 | 10 | 4101075/4289272/4390299 | 800000/4136847/4439973 | 800000/4136846/4437723 | 800000/4226630/4470655 |
| 16 | 10 | 800000/4262161/4486065 | 800000/4245169/4427690 | 800000/4144113/4402794 | 800000/4205473/4315356 |
| 17 | 10 | 4100038/4194982/4311615 | 800000/2449997/4271882 | 800000/4172557/4353807 | 800000/2449988/4247856 |
| 18 | 10 | 800000/4297387/4419642 | 800000/4167587/4436400 | 800000/800000/4381893 | 800000/4162627/4386040 |
| 19 | 10 | 800000/4288879/4410971 | 800000/800000/4403309 | 800000/4146981/4411681 | 800000/2450011/4405280 |
| 20 | 10 | 4104385/4269270/4500273 | 800000/4257800/4529231 | 800000/4247416/4447799 | 800000/800000/4174050 |
| all | | 800000/4269631/4531688 | 800000/4175728/4529231 | 800000/4121754/4447799 | 800000/4100018/4503177 |

Per-run p99 of every line (performance runs 1-20) and the half-split spreads for all lines are in the summary above. For the own-context line: 19616, 20768, 19840, 19904, 19520, 19744, 24448, 22208, 19648, 19456 | 19552, 19424, 19552, 19744, 19776, 19616, 21984, 19712, 19968, 20032. Other lines, p99 spread for runs 1-10 / 11-20: 2 AS passive 6.6 / 17.0 % (run 20 = 2016); 1 AS passive 8.1 / 10.6 %; 4 KiB 9.0 / 7.0 %; contended 9.2 / 6.7 %. All p50 spreads are ≤ 4.2 % (contended line: 3.5 / 4.2 %; over 20 runs 4.5 %).

Ruler columns: "A min-max" is the range across runs of the smallest ruler reading ("took A"), and "B min-max" is the range of the largest ("to B"). Larger values mean a slower CPU.

## Frequency samples, caveats

- Only ~10 samples per boot, because QEMU runs only ~2 s. The samples cannot be lined up with individual measurements.
- Many samples read exactly 800 000 kHz (the minimum) even under `performance`. With intel_pstate, `scaling_cur_freq` comes from APERF/MPERF over the last interval, so a vCPU that was halted or idle at that moment probably reads as minimum. This shows idle, not a slow clock while working. The rulers in the benchmark output are the better evidence that the CPU was fast during the measurements. Non-minimum samples were 4.1-4.53 GHz.

## What could not be done / notes

- The powersave runs were not repeated here, so the comparison is with the given 10 runs from a different time.
- The cause of the own-context p99 outliers (runs 7, 8, 17) was not investigated. No KVM stats (QMP) were collected.
- Nothing was changed outside this directory. runiso.sh used its usual temporary `/srv/joist/test/lockstudy/run.PID` directory and removed it itself. My temporary directory `/srv/joist/test/benchfix` was removed.

Files: `raw/fast-N.log` (boot log with campaign header), `raw/fast-N.freq`, `raw/campaign.log`, `raw/tables.md`, `tools/`.
