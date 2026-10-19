# Bench fix, run 2: IPC benchmark with performance governor + IRQs and build VM steered off CPUs 2-5

Short answer: **the own-context p99 outliers from run1 did not come back, and the null call stayed stable.** In 20 boots, "two address spaces, server with its own context" had p99 19 392-19 904 (spread 2.6 % over all 20 runs, 2.3 % in runs 1-10, 2.6 % in runs 11-20). In run1 it was 19 424-24 448 (25.4 % / 25.2 % / 13.0 %). "A system call that does nothing" had p50 528-532 and p99 572-576 (spread 0.7 %). No run had a slow first measurement: the null-call ruler A was 6 770-7 244 and B was at most 7 612.

**One borderline result:** the contended line ("every other CPU making system calls") has a p50 spread of **5.2 %** over runs 1-10 (8 992-9 472) and over all 20 runs. That is just over the plan's 5 % limit. Runs 11-20 give 4.5 %. In run1 this line had 3.5 % / 4.2 % (4.5 % over 20), so it is close to the limit both times. Its p99 is within the 20 % limit (6.9 % / 10.2 % in the two halves, 11.9 % over all 20). Every other line meets the criterion in both halves.

**Interrupts on CPUs 2-5 during a boot are almost all the host's own:** 3 375-5 057 per boot. Of these, 92 % are the local timer (LOC), and most of the rest are function-call IPIs (CAL) and TLB shootdowns. Device interrupts are nearly zero: over all 20 boots there was **1** device interrupt on CPUs 2-5 (nvme0q5, a managed queue IRQ pinned to CPU 4). I have no IRQ counts from run1, so I cannot show that steering IRQs is what removed the own-context outliers. One campaign before and one after is also too little to tell this apart from chance (run1 had 3 outliers in 20 runs).

## Settings (verified, not changed)

Checked with `tools/check.sh` before the campaign (`raw/settings-before.txt`, 14:15:40 UTC), before every boot (inside `campaign.sh`, which would stop if a check failed; none did), and after the campaign (`raw/settings-after.txt`, 14:18:35 UTC). Before and after give the same result, except that the build VM's QEMU had 11 threads before and 14 after. All of its threads, including the new ones, were on 0,1,6,7.

- Governor: CPUs 2-5 `performance` (CPUs 0,1,6-11 `powersave`).
- `/proc/irq/default_smp_affinity` = `0c3` (= c3: CPUs 0,1,6,7).
- Every other IRQ listed in /proc/irq has smp_affinity_list `0-1,6-7`. These IRQs still include any of CPUs 2-5 or 8-11 (smp_affinity_list / effective):
  - 134 nvme0q3 → 2 / 2; 135 nvme0q4 → 3 / 3; 136 nvme0q5 → 4 / 4; 137 nvme0q6 → 5 / 5 (managed per-CPU NVMe queues)
  - 140 nvme0q9 → 8; 141 nvme0q10 → 9; 142 nvme0q11 → 10; 143 nvme0q12 → 11 (managed per-CPU NVMe queues)
  - 179 i2c-INT3515:00-tps6598x.0 → 0-11 / (no effective CPU); 196 elan_i2c (dummy chip) → 0-11 / (none); 2 (no /proc/interrupts entry, the cascade) → 0-11 / (none). All three had 0 counts.
  - Note: IRQs 121/122 (dmar0/dmar1) have affinity 0-1,6-7, but at the first look their effective_affinity_list was 2 and 3. Their counts were 0. check.sh only tests smp_affinity_list, so it does not flag this.
- Build VM QEMU pid 9937 (`-name joist-dev2`): `taskset -cp` gives 0,1,6,7, and `taskset -acp` gives 0,1,6,7 for every thread.

## Method (same as run1)

- Image `iso/fast.iso`, SHA-256 e9c79ea708187202f153af3517e0c6741dd35ae5402152bb1b1587161fec9d5c (checked).
- Driver `tools/campaign.sh` is run1's script with three additions: a `tools/check.sh` call before each boot, a copy of `/proc/interrupts` right before runiso.sh starts, and another right after it exits (`raw/fast-N.irq-before`/`-after`). Booting is unchanged: `/srv/joist/handoff/lockstudy/tools/runiso.sh`, q35/KVM, SeaBIOS, 4 vCPUs pinned to host CPUs 2-5 (each log's `pin:` lines show CPU n/KVM → 2+n in all 20 runs), under `flock /srv/joist/test/lock`, timeout 400 s.
- Idle gate as in run1: the build VM must be < 2 % busy over 5 s before a boot, and a boot is kept only if the VM stayed < 2 % busy during it. All 20 passed on the first try (0.2-0.4 %), none were discarded, and all ended with `joist: finish code 0`. The governor was checked before and after each boot (`raw/campaign.log`).
- Analysis: `tools/analyze.py` (run1's, unchanged) → `raw/tables.md`. `tools/extra.py` (new) → `raw/extra.md`: all lines for run1 vs run2 over 20 runs and each half, per-run own-context values, and interrupt diffs. The interrupt diff is after minus before, summed over columns CPU2-CPU5, for every row of /proc/interrupts.
- Time: 2026-10-15 14:15:52-14:18:25 UTC, boots back to back. Each interrupt window (runiso.sh lifetime) was about 2.1 s, of which QEMU ran about 2 s.

## Tables

### run2: performance governor, IRQs and build VM steered off CPUs 2-5 (20 runs)

| measurement | p50 median | p50 min-max | p50 spread | p99 median | p99 min-max | p99 spread | ruler A min-max | ruler B min-max |
|---|---|---|---|---|---|---|---|---|
| a system call that does nothing | 528 | 528-532 | 0.8 % | 576 | 572-576 | 0.7 % | 6770-7244 | 6942-7612 |
| two address spaces, passive server | 1624 | 1624-1628 | 0.2 % | 1808 | 1712-1828 | 6.4 % | 6770-6922 | 7436-7634 |
| one address space, passive server | 1170 | 1168-1172 | 0.3 % | 1212 | 1180-1312 | 10.9 % | 6770-7244 | 7102-7626 |
| two address spaces, server with its own context | 14048 | 13888-14176 | 2.1 % | 19648 | 19392-19904 | 2.6 % | 6772-7080 | 7274-7642 |
| two address spaces, passive server, 4 KiB of IPC buffer each way | 2976 | 2976-2984 | 0.3 % | 3112 | 2992-3192 | 6.4 % | 6920-7080 | 7260-7626 |
| two address spaces, passive server, every other CPU making system calls | 9232 | 8992-9472 | 5.2 % | 10224 | 9792-11008 | 11.9 % | 7242-7416 | 7608-7632 |

### powersave governor (given) (10 runs)

| measurement | p50 median | p50 min-max | p50 spread | p99 median | p99 min-max | p99 spread | ruler A min-max | ruler B min-max |
|---|---|---|---|---|---|---|---|---|
| a system call that does nothing | 528 | 528-532 | 0.8 % | 572 | 572-740 | 29.4 % | 6770-9738 | 39100-39156 |
| two address spaces, passive server | 1624 | 1620-1624 | 0.2 % | 1662 | 1636-1696 | 3.6 % | 6770-7242 | 6946-8692 |
| one address space, passive server | 1164 | 1164-1164 | 0.0 % | 1196 | 1172-1248 | 6.4 % | 6770-6922 | 6940-7436 |
| two address spaces, server with its own context | 13808 | 13760-14048 | 2.1 % | 19760 | 19392-22368 | 15.1 % | 6770-6922 | 7086-7618 |
| two address spaces, passive server, 4 KiB of IPC buffer each way | 2976 | 2968-2976 | 0.3 % | 3040 | 3000-3200 | 6.6 % | 6770-7078 | 6954-7616 |
| two address spaces, passive server, every other CPU making system calls | 9072 | 8768-9536 | 8.5 % | 9872 | 9504-10464 | 9.7 % | 7242-7244 | 7438-7622 |

### Every line, run1 vs run2: p50 median / min-max / spread | p99 median / min-max / spread

| line | runs | run1 p50 | run1 p99 | run2 p50 | run2 p99 |
|---|---|---|---|---|---|
| null call | 1-20 | 528 / 528-532 / 0.8 % | 576 / 576-576 / 0.0 % | 528 / 528-532 / 0.8 % | 576 / 572-576 / 0.7 % |
| null call | 1-10 | 528 / 528-532 / 0.8 % | 576 / 576-576 / 0.0 % | 528 / 528-532 / 0.8 % | 576 / 572-576 / 0.7 % |
| null call | 11-20 | 528 / 528-532 / 0.8 % | 576 / 576-576 / 0.0 % | 528 / 528-532 / 0.8 % | 576 / 576-576 / 0.0 % |
| 2 AS passive | 1-20 | 1626 / 1624-1628 / 0.2 % | 1824 / 1708-2016 / 16.9 % | 1624 / 1624-1628 / 0.2 % | 1808 / 1712-1828 / 6.4 % |
| 2 AS passive | 1-10 | 1624 / 1624-1628 / 0.2 % | 1824 / 1708-1828 / 6.6 % | 1624 / 1624-1628 / 0.2 % | 1808 / 1712-1828 / 6.4 % |
| 2 AS passive | 11-20 | 1628 / 1624-1628 / 0.2 % | 1810 / 1708-2016 / 17.0 % | 1624 / 1624-1628 / 0.2 % | 1808 / 1748-1828 / 4.4 % |
| 1 AS passive | 1-20 | 1172 / 1164-1172 / 0.7 % | 1204 / 1180-1308 / 10.6 % | 1170 / 1168-1172 / 0.3 % | 1212 / 1180-1312 / 10.9 % |
| 1 AS passive | 1-10 | 1172 / 1164-1172 / 0.7 % | 1228 / 1180-1280 / 8.1 % | 1172 / 1168-1172 / 0.3 % | 1212 / 1180-1312 / 10.9 % |
| 1 AS passive | 11-20 | 1172 / 1168-1172 / 0.3 % | 1202 / 1180-1308 / 10.6 % | 1168 / 1168-1172 / 0.3 % | 1212 / 1180-1256 / 6.3 % |
| 2 AS own ctx | 1-20 | 14048 / 13856-14240 / 2.7 % | 19744 / 19424-24448 / 25.4 % | 14048 / 13888-14176 / 2.1 % | 19648 / 19392-19904 / 2.6 % |
| 2 AS own ctx | 1-10 | 14048 / 13920-14240 / 2.3 % | 19792 / 19456-24448 / 25.2 % | 14032 / 13920-14176 / 1.8 % | 19616 / 19456-19904 / 2.3 % |
| 2 AS own ctx | 11-20 | 14064 / 13856-14144 / 2.0 % | 19728 / 19424-21984 / 13.0 % | 14064 / 13888-14144 / 1.8 % | 19696 / 19392-19904 / 2.6 % |
| 2 AS 4 KiB | 1-20 | 2976 / 2976-2984 / 0.3 % | 3096 / 2992-3272 / 9.0 % | 2976 / 2976-2984 / 0.3 % | 3112 / 2992-3192 / 6.4 % |
| 2 AS 4 KiB | 1-10 | 2976 / 2976-2984 / 0.3 % | 3124 / 2992-3272 / 9.0 % | 2976 / 2976-2984 / 0.3 % | 3120 / 2992-3192 / 6.4 % |
| 2 AS 4 KiB | 11-20 | 2976 / 2976-2984 / 0.3 % | 3068 / 3000-3216 / 7.0 % | 2976 / 2976-2976 / 0.0 % | 3088 / 3000-3192 / 6.2 % |
| contended | 1-20 | 9184 / 8960-9376 / 4.5 % | 10064 / 9856-10784 / 9.2 % | 9232 / 8992-9472 / 5.2 % | 10224 / 9792-11008 / 11.9 % |
| contended | 1-10 | 9184 / 8960-9280 / 3.5 % | 10080 / 9856-10784 / 9.2 % | 9248 / 8992-9472 / 5.2 % | 10176 / 9792-10496 / 6.9 % |
| contended | 11-20 | 9120 / 8992-9376 / 4.2 % | 10000 / 9888-10560 / 6.7 % | 9232 / 9056-9472 / 4.5 % | 10320 / 9952-11008 / 10.2 % |

### Per run, run2: null, contended, own context (p50 / p99), own-context ruler A-B

| run | null p50 | null p99 | contended p50 | contended p99 | own ctx p50 | own ctx p99 | own ctx ruler A-B | IRQs on CPUs 2-5 |
|---|---|---|---|---|---|---|---|---|
| 1 | 532 | 576 | 9472 | 10080 | 13984 | 19584 | 7078-7608 | 3776 (LOC 3605, CAL 96, TLB 59, RES 16) |
| 2 | 528 | 576 | 9312 | 10496 | 13984 | 19840 | 6924-7642 | 4527 (LOC 4065, CAL 278, TLB 158, RES 24) |
| 3 | 528 | 576 | 9216 | 10208 | 14144 | 19904 | 6924-7616 | 4232 (LOC 3892, CAL 207, TLB 116, RES 13) |
| 4 | 528 | 576 | 9344 | 10144 | 14176 | 19616 | 6922-7274 | 4065 (LOC 3715, CAL 215, TLB 121, RES 14) |
| 5 | 532 | 576 | 9152 | 10336 | 14048 | 19616 | 6776-7622 | 5057 (LOC 4603, CAL 270, TLB 162, RES 20) |
| 6 | 528 | 576 | 9184 | 10016 | 14080 | 19744 | 6922-7276 | 3952 (LOC 3621, CAL 205, TLB 102, RES 24) |
| 7 | 532 | 576 | 8992 | 9792 | 13920 | 19488 | 6772-7598 | 3499 (LOC 3387, CAL 73, TLB 28, RES 11) |
| 8 | 532 | 572 | 9280 | 10240 | 14016 | 19456 | 6922-7622 | 3785 (LOC 3636, CAL 92, TLB 38, RES 19) |
| 9 | 528 | 576 | 9216 | 10048 | 13952 | 19456 | 6922-7626 | 3957 (LOC 3524, CAL 261, TLB 146, RES 26) |
| 10 | 528 | 576 | 9376 | 10304 | 14048 | 19744 | 6924-7612 | 4758 (LOC 4330, CAL 262, TLB 136, RES 30) |
| 11 | 532 | 576 | 9216 | 10048 | 14080 | 19456 | 6922-7604 | 3375 (LOC 3252, CAL 70, TLB 38, RES 13) |
| 12 | 528 | 576 | 9440 | 10560 | 13984 | 19424 | 6922-7432 | 3645 (LOC 3358, CAL 187, TLB 78, RES 22) |
| 13 | 528 | 576 | 9472 | 11008 | 14112 | 19712 | 7080-7450 | 3893 (LOC 3581, CAL 192, TLB 99, RES 21) |
| 14 | 532 | 576 | 9248 | 10240 | 14144 | 19776 | 6772-7450 | 3761 (LOC 3368, CAL 235, TLB 121, RES 33) |
| 15 | 528 | 576 | 9376 | 10464 | 14048 | 19808 | 7078-7620 | 3466 (LOC 3253, CAL 131, TLB 67, RES 15) |
| 16 | 532 | 576 | 9152 | 9952 | 14080 | 19680 | 6774-7624 | 3584 (LOC 3454, CAL 78, TLB 37, RES 13) |
| 17 | 528 | 576 | 9152 | 10112 | 14080 | 19904 | 6922-7448 | 4109 (LOC 3598, CAL 303, TLB 187, RES 21) |
| 18 | 528 | 576 | 9056 | 9952 | 13952 | 19808 | 6922-7616 | 4640 (LOC 4238, CAL 230, TLB 142, RES 30) |
| 19 | 532 | 576 | 9312 | 10400 | 13920 | 19392 | 6780-7440 | 4024 (LOC 3671, CAL 209, TLB 129, RES 15) |
| 20 | 528 | 576 | 9120 | 10400 | 13888 | 19456 | 6922-7448 | 3501 (LOC 3158, CAL 204, TLB 111, RES 25) |

(The last column is the total for CPUs 2-5 in that boot, with its top four sources. Own-context ruler A ≥ 6 772 and B ≤ 7 642 in every run, so no slow CPU there.)

For comparison, run1's own-context p99 per run: 19616, 20768, 19840, 19904, 19520, 19744, **24448**, **22208**, 19648, 19456 | 19552, 19424, 19552, 19744, 19776, 19616, **21984**, 19712, 19968, 20032. Run1's null and contended per-run values are in `run1/findings.md`.

### Interrupts on host CPUs 2-5 during the boots, all 20 runs, by source

| source | total | per run min-median-max | CPU2 | CPU3 | CPU4 | CPU5 |
|---|---|---|---|---|---|---|
| LOC Local timer interrupts | 73309 | 3158-3601.5-4603 | 24902 | 16555 | 17605 | 14247 |
| CAL Function call interrupts | 3798 | 70-206-303 | 1307 | 919 | 1023 | 549 |
| TLB TLB shootdowns | 2075 | 28-113.5-187 | 706 | 487 | 597 | 285 |
| RES Rescheduling interrupts | 405 | 11-20.5-33 | 97 | 119 | 97 | 92 |
| NMI Non-maskable interrupts | 9 | 0-0-2 | 3 | 2 | 2 | 2 |
| PMI Performance monitoring interrupts | 9 | 0-0-2 | 3 | 2 | 2 | 2 |
| 136 nvme0q5 | 1 | 0-0-1 | 0 | 0 | 1 | 0 |

How to read this: LOC is the host's own 1000 Hz tick (CONFIG_HZ=1000, no nohz_full/isolcpus), about 900 per CPU per 2.1 s window. IRQ affinity cannot steer it. CAL and TLB are IPIs sent by the host kernel, most likely from QEMU/KVM setting up and tearing down the guest's memory and vCPU threads. Because the window also covers QEMU start and exit, some of them come from outside the benchmark itself. NMI/PMI were 0-2 per boot. Only one device interrupt landed on CPUs 2-5 in all 20 boots.

### Slow first measurement (null call ruler A > 8000 counts)

- performance: none
- powersave: [1, 8]

### Frequency samples (kHz, only while the boot's QEMU ran)

| run | samples | CPU2 min/med/max | CPU3 min/med/max | CPU4 min/med/max | CPU5 min/med/max |
|---|---|---|---|---|---|
| 1 | 10 | 800000/4265277/4395549 | 4122332/4279894/4509570 | 800000/4237656/4400679 | 800000/4145893/4325099 |
| 2 | 10 | 4098628/4275093/4409305 | 800000/4159735/4369833 | 4100005/4196286/4419112 | 800000/4137717/4351238 |
| 3 | 10 | 800000/4305726/4378738 | 800000/4320483/4399019 | 800000/4137492/4454977 | 800000/800000/4322935 |
| 4 | 10 | 4100003/4272310/4515993 | 800000/4231609/4536592 | 4099963/4306789/4435826 | 800000/800000/4232095 |
| 5 | 10 | 800000/4157778/4351873 | 800000/4307826/4436468 | 4099998/4317048/4534356 | 800000/4264676/4399955 |
| 6 | 10 | 4100044/4293459/4574382 | 800000/4217122/4356766 | 800000/4142250/4313984 | 800000/800000/4306179 |
| 7 | 10 | 4103011/4331219/4525243 | 800000/2450015/4328568 | 800000/4237538/4433359 | 800000/4102468/4477666 |
| 8 | 10 | 800000/4272831/4417788 | 800000/4235087/4470115 | 800000/4211755/4366843 | 800000/800000/4342146 |
| 9 | 10 | 4099967/4298306/4522377 | 800000/2449994/4482619 | 800000/4128789/4397097 | 800000/2449987/4266306 |
| 10 | 10 | 4105217/4318550/4409377 | 4100014/4285513/4490549 | 800000/4242214/4487622 | 800000/4111876/4414411 |
| 11 | 10 | 800000/4206858/4377333 | 800000/4127485/4401544 | 800000/4124269/4346780 | 4100008/4308385/4374084 |
| 12 | 10 | 4124045/4322667/4485999 | 800000/4217045/4327724 | 800000/800000/4304753 | 800000/2461786/4550358 |
| 13 | 10 | 4099929/4284660/4524834 | 800000/4122154/4267683 | 800000/4123338/4301935 | 800000/4123219/4370784 |
| 14 | 10 | 4099928/4332083/4371094 | 800000/4121659/4318553 | 800000/4171668/4328745 | 800000/800000/4348761 |
| 15 | 10 | 800000/4234053/4433568 | 800000/4166385/4491555 | 800000/4196168/4441452 | 800000/800000/4229345 |
| 16 | 10 | 4100084/4285646/4415305 | 800000/800000/4407076 | 800000/2449996/4392168 | 800000/4255441/4395805 |
| 17 | 10 | 800000/4248039/4351942 | 800000/800000/4403685 | 800000/4154221/4348351 | 800000/2450010/4288830 |
| 18 | 10 | 800000/4249744/4567443 | 800000/4270251/4495511 | 800000/4238885/4356849 | 800000/4182132/4344231 |
| 19 | 10 | 800000/4295157/4519507 | 800000/4108052/4400574 | 800000/4108146/4307058 | 800000/4108029/4328421 |
| 20 | 10 | 4100550/4316602/4406121 | 800000/4110459/4418047 | 800000/800000/4471183 | 800000/800000/4219331 |
| all | | 800000/4289154/4574382 | 800000/4173053/4536592 | 800000/4178713/4534356 | 800000/4100008/4550358 |

As in run1, many `scaling_cur_freq` samples read 800 000 kHz. These are idle vCPUs (APERF/MPERF), not a slow clock. All other samples were 4.1-4.57 GHz.

## Comparison with run1

- Null call: same as run1 (p50 528-532, p99 576). One run (8) had p99 572. No slow first measurement in either run.
- Own context: run1 had p99 outliers in runs 7, 8 and 17 (24 448 / 22 208 / 21 984), which made the spread 25.4 %. Run2 has none (max 19 904, spread 2.6 %). p50 is the same (median 14 048 in both).
- 2 AS passive: p99 spread 16.9 % → 6.4 %, because run1's outlier (run 20, 2016) did not recur.
- 1 AS passive, 4 KiB: about the same (10.6 → 10.9 %, 9.0 → 6.4 %).
- Contended: slightly worse. p50 median 9 184 → 9 232, p50 spread 4.5 → 5.2 % (runs 1-10: 3.5 → 5.2 %, which fails the < 5 % criterion), p99 median 10 064 → 10 224, p99 spread 9.2 → 11.9 %.
- Settings differ from run1 in two ways: IRQs were steered and the build VM was pinned (in run1 neither was recorded). The build VM was equally idle in both (0.2-0.4 %).

## What could not be done / notes

- Run1 has no interrupt counts, so how much interrupt traffic on CPUs 2-5 fell cannot be measured. This campaign only shows what is left.
- With 3 outliers in 20 runs in run1 and 0 in 20 here, the improvement is suggestive, not proven. Attributing it to IRQ steering or VM pinning would take an A/B test that alternates settings, which would mean changing settings, and that is not my task.
- The interrupt window includes QEMU startup and teardown, so it cannot be limited to the benchmark's measurements.
- Nothing was changed outside this directory. runiso.sh removed its own `/srv/joist/test/lockstudy/run.PID` directories (none left). My temporary `/srv/joist/test/benchfix` was removed.

Files: `raw/fast-N.log`, `raw/fast-N.freq`, `raw/fast-N.irq-before`, `raw/fast-N.irq-after`, `raw/campaign.log`, `raw/settings-before.txt`, `raw/settings-after.txt`, `raw/tables.md`, `raw/extra.md`, `tools/` (campaign.sh, check.sh, analyze.py, extra.py).
