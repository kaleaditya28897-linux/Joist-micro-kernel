# Timer write after the lock's release (decision 0017): measurements and stress (2026-10-15)

Measurements and stress runs only. No source file changed in any tree but
my copy, nothing committed, nothing deleted outside my copy.

## Summary

- **Own-context scaling changes a lot; nothing else measurably does.**
  With the new default (the timer write after the release), 4 own-context
  pairs make **2.91×** what 1 pair makes (old order: **0.84×**). Per round
  trip at K=4: 19 083 cycles new, 67 879 old.
- Passive scaling is unchanged: 0.59× at K=4 in both builds.
- Uncontended costs (ipcbench p50s, and scaleroot passive K=1) are the
  same, or at most 4 to 12 cycles more with the new order. The
  contended-line case is 64 cycles more. In every one of these the two
  builds' ranges overlap.
- **Stress: all 50 boots passed.** That is faultinj seeds 21 to 30 on the
  normal and the checking kernel, plus smp ×5 and sched ×5, all on 4 vCPUs.

## Restart

Before this run, the first attempt's processes were still running: its
`claude -p` (pid 3334), `all.sh` and `campaign.sh`, and a boot of an old
image (`to-new-sc`, round 3) that had just started. I killed them so they
could not boot old images into `raw/` or overwrite `status`. Their files are
not used here:
- the first attempt's logs are in `old/raw/` (left where they were);
- its work files are in `/srv/joist/test/timerout/old1/`;
- its images were set aside unused.

## Method

- **Tree:**
  - On the build VM (`ssh -p 2222 aditya@127.0.0.1`) I made a fresh copy:
    `rm -rf ~/joist-to && cp -a ~/joist-timer ~/joist-to && find ~/joist-to
    -name build -type d -prune -exec rm -rf {} +`.
  - `diff -r -x build ~/joist-timer ~/joist-to` printed nothing (status 0).
  - The copy has `RUN_LOCK_TIMER_OUT` defaulting to 1 (`run.c:192`) and the
    fixed `on_own_stack`.
  - `~/joist-timer` is not a git repository, so no revision can be given.
- **Builds:** in `~/joist-to/kernel` (`tools/build.sh`), with `make -s
  BUILD=build/NAME KEXTRA=… ROOT_TASK=… MODULES=… UEXTRA=… USER_BUILD=…
  build/NAME/joist.iso`. The images:

  | Image | KEXTRA | Root task, modules | UEXTRA (USER_BUILD) |
  |---|---|---|---|
  | `to-new-sc` | (none: new default) | scaleroot, ipcbench | — |
  | `to-old-sc` | `-DRUN_LOCK_TIMER_OUT=0` | scaleroot, ipcbench | — |
  | `to-new-lb` | (none) | benchroot, ipcbench (the bench-gate image) | — |
  | `to-old-lb` | `-DRUN_LOCK_TIMER_OUT=0` | benchroot, ipcbench | — |
  | `fi-N`, N = 21..30 | (none) | faultinj, fiworker | `-DFAULTINJ_SEED=N` (`build/faultinj-N`) |
  | `fichk-N` | `-DRUN_CHECK_INVARIANTS` | faultinj, fiworker | as `fi-N` |
  | `to-smp` | (none) | smproot, worker | — |
  | `to-sched` | (none) | schedroot, worker | — |

  - The new and old kernels' ELFs differ (`cmp`).
  - The lock study's `lb-*` images used `UEXTRA=-DBENCH_CALLS=100000u`.
    These use the bench-gate defaults (`n=20x5000`), as asked.
- **Host:** the P53 (i7-9850H), governor `powersave`, QEMU 11.1.2.
  - Boots use `tools/runiso.sh`: KVM, `-cpu host -smp 4 -m 256M`, SeaBIOS.
  - vCPU *n* is pinned to host CPU 2+*n*, under `flock
    /srv/joist/test/lock`.
  - Each log records the pinning (`pin:` lines).
- **Timing:** `tools/campaign.sh 7 to-new-sc to-old-sc to-new-lb
  to-old-lb`: 7 rounds, with the variants interleaved inside each round.
  - Each boot started only once the build VM was under 2 % busy over 5 s.
    It was kept only if the VM stayed under 2 % for the whole boot.
  - All 28 boots were kept, none discarded. The VM was 0.0 to 0.8 % busy
    during them (`raw/campaign.log`, and each log's first line).
  - Figures are **median (min-max) over the 7 boots** from
    `tools/parse.py`, the lock study's script, unchanged.
  - Cycles are CPU cycles by the clients' ruler.
  - No client started late (`late=0` throughout).
- **Stress:** `tools/stress.sh` ran each boot on 4 vCPUs with a 300 s
  limit. Every boot was kept, whatever the VM was doing (it was idle: the
  other session's check had finished).
  - The pass condition is selftest.sh's for these entries: QEMU status 1
    (root task exit code 0) plus every expected line.
  - faultinj expected lines: call / server / saved / transfer / signal ok,
    "13 of 13 kill points covered", "faultinj: all ok", "root task exited
    with code 0".
  - smp / sched expected lines: "cpus in use: 4", "smp: all ok" / "sched:
    all ok", and "root task exited with code 0".

## 1. Scaling (scaleroot): round trips/s in total, median (min-max) over 7 boots

| Mode | K | New (timer write after release) | Old order | Scaling vs K=1, new / old | Cycles per round trip, new | Cycles per round trip, old |
|---|---|---|---|---|---|---|
| passive | 1 | 2 613 102 (2 580 393-2 632 926) | 2 596 062 (2 588 262-2 639 935) | 1.00 / 1.00 | 1 723 (1 695-1 733) | 1 711 (1 691-1 718) |
| passive | 2 | 1 888 306 (1 613 524-1 941 282) | 1 885 375 (1 733 153-1 940 221) | 0.72 / 0.73 | 4 653 (4 531-5 399) | 4 628 (4 503-5 017) |
| passive | 3 | 1 693 614 (1 620 766-1 758 416) | 1 670 973 (1 626 394-1 715 156) | 0.65 / 0.64 | 7 681 (7 410-7 876) | 7 745 (7 582-7 862) |
| passive | 4 | 1 531 192 (1 463 665-1 548 974) | 1 520 707 (1 467 102-1 546 505) | 0.59 / 0.59 | 11 035 (10 999-11 419) | 11 002 (10 844-11 398) |
| own-context | 1 | 289 687 (288 686-296 144) | 296 337 (293 270-300 825) | 1.00 / 1.00 | 15 124 (15 039-15 407) | 14 994 (14 775-15 076) |
| own-context | 2 | 502 665 (497 465-509 634) | 264 314 (262 017-267 645) | **1.74** / 0.89 | 17 193 (17 034-17 371) | 32 823 (32 703-33 189) |
| own-context | 3 | 718 230 (714 872-725 502) | 257 418 (252 414-258 949) | **2.48** / 0.87 | 17 174 (17 008-17 650) | 49 820 (48 921-50 405) |
| own-context | 4 | 842 163 (833 793-846 904) | 248 130 (243 102-249 348) | **2.91** / 0.84 | 19 083 (18 855-19 439) | 67 879 (67 267-68 672) |

Per pair, own-context:
- new: 289 687, 251 332, 239 410, 210 541 per second (K = 1 to 4);
- old: 296 337, 132 157, 85 806, 62 032.

Fairness: the least/most pair ratio was 0.99 to 1.00 everywhere.

CPU cycles per TSC count fell as more cores were busy: 1.73 to 1.55 (new),
1.72 to 1.61 (old). The cycle columns allow for this; the per-second
columns do not.

**Compared with the lock study:**
- The old-order figures repeat its findings: own-context 0.85× and passive
  0.57× at K=4.
- Today's passive K=1 costs more per round trip: 1 711 cycles today against
  about 1 527 then.
- The cause of that difference was not looked into. It affects both of
  today's builds equally.

## 1b. ipcbench uncontended (bench-gate image): p50 (p99) cycles, median (min-max) over 7 boots

| Case | New p50 | Old p50 | New p99 | Old p99 |
|---|---|---|---|---|
| null system call | 528 (528-528) | 528 (528-532) | 576 (576-600) | 572 (572-748) |
| two address spaces, passive server | 1 624 (1 624-1 628) | 1 620 (1 620-1 624) | 1 668 (1 664-1 744) | 1 664 (1 636-1 696) |
| one address space, passive server | 1 172 (1 168-1 172) | 1 164 (1 164-1 164) | 1 180 (1 176-1 200) | 1 172 (1 172-1 192) |
| two address spaces, own-context server | 14 016 (13 952-14 080) | 13 984 (13 792-14 144) | 19 680 (19 456-20 416) | 20 320 (19 616-22 432) |
| passive, 4 KiB of IPC buffer each way | 2 976 (2 976-2 976) | 2 976 (2 976-2 976) | 3 000 (3 000-3 048) | 3 048 (2 992-3 056) |
| (contended) passive, every other CPU making system calls | 9 024 (8 960-9 408) | 8 960 (8 864-9 248) | 9 888 (9 728-10 400) | 9 920 (9 600-10 080) |

**Uncontended differences (new minus old, p50):**
- null call: 0;
- passive round trip: +4 (two spaces), +8 (one space);
- own-context: +32 (the ranges overlap);
- IPC buffer: 0.

**Lower uncontended figures than the lock study's.** The lock study
measured 380 / 1 448 / 988 for the null call, two spaces and one space;
today's are 528 / 1 624 / 1 172.
- Its `lb-*` images were built from a different tree, with
  `BENCH_CALLS=100000u`.
- I did not look into the difference. It applies equally to both of
  today's variants.

## 2. Stress: every boot (4 vCPUs, P53)

All 50 boots: **ok**. QEMU status 1 every time, and every expected line was
present. Durations are from `raw/stress.txt`.

| Image | Verdict | Time |
|---|---|---|
| fi-21 … fi-30 (each once) | ok ×10, "faultinj: all ok", "13 of 13 kill points covered" | 5-6 s |
| fichk-21 … fichk-30 (checking kernel, each once) | ok ×10, as above | 16-17 s |
| to-smp, 5 boots | ok ×5, "smp: all ok" | 2-3 s |
| to-sched, 5 boots | ok ×5, "sched: all ok" | 2-3 s |

The per-boot lines (`fi-21 run 1: ok (QEMU status 1, 5s)`, …) are in
`raw/stress.txt`. The logs are `raw/stress-IMAGE-RUN.log`. There were no
failures, so there are no failure logs to give.

## Image SHA-256s (`raw/images.sha256`)

They are the same on the VM (`~/joist-to/kernel/build/*/joist.iso`) and on
the P53 (`/srv/joist/test/timerout/iso/`).

```
ff45f54b69ed2487bf90224f420e174c2fe91d2fcbec19e152ed0c978e21c469  fi-21
4bbff53a92324b391e5c19c71d0db83ed25f4e931880e0bd6e6a513fb89dafed  fi-22
30094fe79b33aa16692b3d3717046a8a09168c6185f43d42a9aa862c67bf1fed  fi-23
4823a591514c4107abbb6766406d8a95609ee18122182e80f913c71dddffaecd  fi-24
53f171a88a4a217ae008c67200a297ecc9693a779f81cb8caa002485939f0589  fi-25
ebead0088524dc56a9af387af7859837861c9ace02d440b8d34898816966f439  fi-26
f88568da949b5b203ee3a0b4219fa54298410163b88ca07e883439e6e37899af  fi-27
26dffdc52defc0aff63ab678f9139398d7cd0b619ea7d14d15e5d7d2e1a51297  fi-28
62bec53e75d1d6524c804532a63ece6d43cc8f59557aed7ab6bad058f33f62ea  fi-29
4ebde3143e24a14d81155da53174af8dc0adee11a7064b5fbf2b9fad0de80530  fi-30
d0f0da8c6675accac6164a937c50088bf9ff7d416123cf719f1fd45260f9f241  fichk-21
064ccda0cbe02c2564314bdb56d28b3a75aafc9867f39656059ccde37d5b2666  fichk-22
a3a9ae55795155dbd1eb7a5c2fb1d5cd3fe70fd070024adb0624418b09fccae0  fichk-23
d951b00fce7c21b72173c52edbedeb4fe0d906f05e211feeffb5ccf7d657a080  fichk-24
9da725d2211ccac78af90a15dca703ef74f6c938fdf8aec012299e8f4e699ec1  fichk-25
23bb07599d633f18116811e6b6a72e4a033532ab2c46bc09163de212abdeb1e6  fichk-26
f7eeee126a312dc0433eff6b180ee2eca2ad416909fac9d2059e6ab01a432836  fichk-27
3053b1bccae8695db77c6ff75afff1ec839bd3af1bce60c4ac22d94ba14838fc  fichk-28
106b09315f0f6cb08fe008f0d30a1e0add56be5f0bf3b8b842532e7fbf0f2252  fichk-29
89f62c4796a168d8f2e676d4c1d0162db86e32802312547fb340dfdeec92b0dc  fichk-30
cb761ead113c63675b50dc59ee78f7eadd789e35f6a26adba128001c32943aeb  to-new-lb
023a6f8e37d41cbad72b2a1c46344a1fde75f4eefa02a5e32a462da6bc207ff6  to-new-sc
40c6ba1848bb36c11821c6946e5d218dc7c23117bc619513607013043fb2ebd2  to-old-lb
17360e618d79129eb7d4dd8d7b631bf6b1d9f9e4b7c1a8cd9556176c240a9699  to-old-sc
0bd380766a10ab3292c87fa54cc28e2acdbb5da83ee9220e4ea98cb7f9cb8c72  to-sched
71b30aec5a4f7e014947feabaea0047d0153389c7e44a55ea2c3869b0b1e4d0d  to-smp
```

## What could not be done, or was done differently

- **"The larger sizes selftest.sh uses for chk- entries": selftest.sh has
  none.** Its `chk-` prefix adds only `-DRUN_CHECK_INVARIANTS` to KEXTRA.
  - The one chk-specific change is a smaller workload for
    `chk-*pingpong-buf` (`PING_ROUNDS=2000u`).
  - The larger sizes (`$big`: `JOIST_MAX_OBJECTS=8192u` etc.) apply only to
    the `cascade` entries.
  - Its own `chk-kvm-smp4-faultinj-7` is built with the default KSIZES, so
    the `fichk-N` images were too. If other sizes were meant, these boots
    do not cover them.
- No lock-statistics builds (`-DRUN_LOCK_STATS`) were made. Why own-context
  scaling changes this much is therefore not measured here, only that it
  does.
- No revision id for the tree: `~/joist-timer` is not a git repository.
  The fresh copy and the empty `diff` are what identify it.

## Files

- `tools/`: the lock study's `runiso.sh`, `campaign.sh`, `parse.py` and
  `qmpstats.py` (with paths changed to `timerout`). Also `build.sh`
  (builds every image on the VM), `stress.sh` and `timing.sh`.
- `raw/`:
  - `to-{new,old}-{sc,lb}-{1..7}.log`: the 28 kept timing boots;
  - `stress-*.log`: the 50 stress boots;
  - `campaign.log`, `stress.txt` and `images.sha256`.
- `old/`: the stopped first attempt's logs, not used.
