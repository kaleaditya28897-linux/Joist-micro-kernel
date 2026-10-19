# The contended line under 0018: is placement fixed, and what spreads it? (2026-10-19)

Measurements only. I changed nothing outside my copy `~/joist-ct3` on the build VM, committed nothing and touched no system setting. All boots went through `boot.sh.current`'s remote half: fixed zeroed `/dev/shm` RAM, QEMU on 0,1,6,7, vCPUs on 2-5, the build VM narrowed to 0,1,6,7 for each boot and put back to 0,1,6-11 after. The 2 % idle gate held for every boot: 430 kept, 0 discarded.

## Short answers

1. **No. Under 0018 the client still lands on a different vCPU from boot to boot.**
   - Over 150 single-measurement instrumented boots on the standard order, the client was on vCPU 0 / 1 / 2 / 3 in 14 / 42 / 53 / 41 boots.
   - The busy threads always hold the other three CPUs.
   - 0018 itself does what it says *for idle CPUs*. The three busy threads go to the lowest idle CPUs in order, every time: 1, 2, 3 when the root is on CPU 0, which it is in 242 of 300 boots.
2. **Placement is a minority of the spread now. Within one placement the spread is larger than the 1.7 % residual.**
   - **Placement's share.** In each image and host order, the client's vCPU explains 5-52 % of the boot-to-boot variance (median ~23 %).
   - **SD within one placement: 1.9-3.1 %** in every image (light-ish `place` build: 3.0 %). That is not the residual 1.7 %; it is larger.
   - **10-boot windows within one placement.** These run 2.3-11.6 %. Only 4 of 17 (image, placement) groups with ≥ 10 boots are under 5 %, and which placement is stable differs from image to image. There is no consistently good or bad placement.
   - **Host cores.**
     - Rotating which host core each vCPU runs on (4 orders × 40 depth boots) shifts the median by up to 3.1 % (6 976-7 200).
     - Every order spreads 13.5-14.4 %.
     - The client's *host* core explains ~2 % of the variance. Its *vCPU number* explains ~21 % (vCPU 1 +2.0 %, vCPU 3 −1.3 %, pooled over orders, the same sign as contend2).
     - So what placement effect there is follows the guest's CPU numbering, not a good or bad pair of host cores.
   - **Host-side figures.** None of these explains the spread: the ruler (core clock) r = −0.2 to +0.3; host frequency at the end; package temperature r = −0.4 to +0.4, inconsistent sign; host IRQs on CPUs 2-5.
     - The only steady correlate is the host timer tick count (LOC) on CPUs 2-5, r = +0.25 to +0.76.
     - Its slope is what a longer contended phase produces at HZ = 1000: 0.08-0.12 ticks per cycle of p50 against 0.096 expected; 0.25 vs 0.29 for the 3× longer image. So it is an effect of the spread, not a cause.
   - **Per-boot vs per-measurement.** Three contended measurements in one boot (`rep`, gate kernel) agree poorly: ICC 0.22, per-measurement SD 2.5 %, per-boot SD 1.3 %. With the placement log (`rep-place`): ICC 0.63, 2.2 % and 2.8 %.
     - Inside a boot, two measurements with the *same* client CPU differ as much as two with different ones (2.5 % vs 2.2 % mean |difference|).
     - So there is a per-boot part *and* a per-measurement part of similar size, and neither is placement.
   - **Longer rounds.** 3× longer rounds (`long`) cut the within-boot round SD from 1.7 % (`base`) to 1.0 %, but not the SD of boot means (1.5 % for `base`, 1.7 % for `long`; CV over all boots 1.7 % gate, 2.0 % long). Whatever sets a run's level holds for at least the whole measurement (≥ 0.6 s).
   - **Lock order.** The order the CPUs take the lock in is not a frozen per-boot state.
     - Ticket gaps: a busy CPU overtakes another about once a round (median 1, at most 6 per CPU per round), but each CPU's predecessor changes within every round (dominant predecessor share median 0.57).
     - The busy CPUs' cyclic direction moves the round p50 by only about ±0.5 %.
   - **Code layout.** Kernel changes that add nothing to the timed path shift the line's level by several per cent: `order` −3.9 %, `depth` +2.3 %, `place` +2.5 % against gate. The light images (gate, base, long, rep) all share the gate's kernel `.text`.
3. **Why placement is not fixed.** It is a race that 0018 does not cover: the client is woken while every other CPU is busy.
   - **The root is preempted while all CPUs are busy.** The root (priority 128) binds the server's context (priority 200) on its own CPU. The server preempts the root there. The root is now ready, and every other CPU is running a busy thread (priority 100), so it should preempt one of them.
   - **0018 pokes one CPU, but the claim isn't binding.** `joist_sched_pokes` pokes the busy CPU whose thread goes last (CPU 3 in 283 of 300 boots). But the busy CPUs are in the kernel lock's queue continuously (the convoy), and whichever of them reaches its next choice first takes the root.
     - The poked CPU won in only 89 of 300 boots (30 %).
     - In 211 (70 %), another busy CPU took the root while the poke to CPU 3 was still in flight. Its choice was made with `inflight` = 8 (or 4): the decision's "a CPU may have taken a thread another had claimed".
   - **The client starts where the root is.** The client's context is bound by the root and the client starts where the root is when the root blocks in its Recv. That was the root's taker in 259 of 270 single-measurement boots; the other 11 were cascades that returned the root to its first CPU.
   - **The root's own start varies too.** The root's CPU at the start of the contended setup is also not fixed (CPU 0 in 242 boots, 1/2/3 in 58), from the same kind of race in earlier measurements.
   - Event logs: `raw/*.log`, lines `placement:`.
4. **Against the criterion.**
   - **What the criterion needs.** With normally distributed boot medians, passing *every* 10-boot window of a 20-boot campaign most of the time needs a CV of about 1.0-1.2 %. At CV 1.2 % it passes 78 % of the time; at 1.5 % 35 %; at 1.7 % 16 % (`tools/criterion_sim.py`).
   - **Where the gate is.** The gate image here: CV 1.7 %, 40 boots 6 928-7 488 (8.0 %), worst 10-window 8.0 %. That matches the 40-boot campaign's b (7 096, 11.3 %).
   - **Making placement binding** (only the claimed CPU may take the root, or the root not leaving its CPU) would make placement follow events. It would remove at most the placement share of the variance: 5-52 %, about a fifth in the near-light build.
     - The within-placement SD it leaves is 1.9-3.1 % in every instrumented image. That is above the ~1.2 % needed.
     - On these numbers it would not plausibly bring the line under 5 % by itself.
   - **The remainder.**
     - A per-measurement level, set when a measurement starts and held for its length, not tied to placement or to the lock order.
     - A per-boot level of similar size.
     - A sensitivity to kernel code layout of several per cent.
     - I found no host-side variable that tracks it (host cores, clock, temperature, host interrupts). Equally, I found nothing guest-side that does.
     - I could not test host per-boot allocations (KVM's EPT/VMCS pages, whether tmpfs pages of the RAM file move) without root. They are the remaining host-side candidates for the per-boot part. The per-measurement part, being different within one QEMU process, is not one of them.
   - **Repeating the measurement in a boot**, as the decision's option 3 suggests, did not help here: the median of 3 per boot spread 9.0 % (`rep`). Measurements 2 and 3 were noisier than the first (CV 2.9-3.5 % vs 1.3 %).

Boots ran 13:10-14:15 (local time), about 65 minutes including smoke boots, inside the 90-minute budget.

## Placement: what happens, in order (one boot, `depth@2345-1`)

From the placement log (`a` = thread: 0 root, 1-3 busy, 4 server, 5 client, 32 none):

```
enter cpu 0 a 0 / cpus 1-3 idle            root on CPU 0 at the start
poke cpu 0 a 2 ; enter cpu 1 a 1           busy 1 -> lowest idle CPU (0018: one CPU per thread)
poke cpu 0 a 4 ; enter cpu 2 a 2           busy 2 -> CPU 2
poke cpu 0 a 8 ; enter cpu 3 a 3           busy 3 -> CPU 3
mark 30 ; poke cpu 0 a 8 ; enter cpu 0 a 4 server (prio 200) preempts the root on CPU 0; CPU 3 poked for the root
poke cpu 1 tag 8 ; enter cpu 1 a 0         CPU 1 (busy, in the lock queue) chooses first, CPU 3's poke in flight: takes the root
enter cpu 3 a 1 ; enter cpu 0 a 3          busy threads shuffle
enter cpu 1 a 4 ... mark 32 ; enter cpu 1 a 5   handshake, then the client starts on CPU 1
```

In the boots where CPU 3 wins, the same sequence ends with the root and the client on CPU 3.

| who took the root after the server's bind | boots |
|---|---|
| the poked CPU (inflight 0 when it chose) | 89 |
| another busy CPU, poke to CPU 3 in flight (inflight 8) | 201 |
| another busy CPU, poke to CPU 2 in flight (inflight 4) | 10 |

| root's CPU at the start of the contended setup | boots | busy threads placed on |
|---|---|---|
| 0 | 242 | 1, 2, 3 |
| 1 | 21 | 0, 2, 3 |
| 2 | 20 | 0, 1, 3 |
| 3 | 17 | 0, 1, 2 |

## Every image (contended line; `rep*`: the last of the three measurements)

| variant | boots | p50 median | min-max | spread | worst 10-window | CV | p99 median / spread | worst-round median |
|---|---|---|---|---|---|---|---|---|
| gate@2345 | 40 | 7040 | 6928-7488 | 8.0 % | 8.0 % | 1.7 % | 7904 / 7.9 % | 7312 |
| base@2345 | 20 | 7048 | 6592-7120 | 7.5 % | 7.5 % | 1.6 % | 7912 / 10.3 % | 7336 |
| long@2345 | 40 | 7016 | 6608-7216 | 8.7 % | 8.7 % | 2.0 % | 7848 / 11.0 % | 7184 |
| rep@2345 | 30 | 7064 | 6448-7280 | 11.8 % | 10.6 % | 3.5 % | 7760 / 12.6 % | 7256 |
| place@2345 | 40 | 7216 | 6528-7712 | 16.4 % | 16.5 % | 3.2 % | 8176 / 20.7 % | 7576 |
| rep-place@2345 | 30 | 7248 | 6576-7472 | 12.4 % | 12.4 % | 4.1 % | 8016 / 12.0 % | 7416 |
| depth@2345 | 40 | 7200 | 6688-7584 | 12.4 % | 12.5 % | 2.3 % | 8216 / 15.6 % | 7648 |
| depth@3452 | 40 | 6976 | 6496-7504 | 14.4 % | 14.4 % | 3.3 % | 8000 / 15.4 % | 7432 |
| depth@4523 | 40 | 7104 | 6592-7552 | 13.5 % | 13.7 % | 3.5 % | 8080 / 14.7 % | 7584 |
| depth@5234 | 40 | 7168 | 6656-7632 | 13.6 % | 14.0 % | 3.2 % | 8152 / 14.5 % | 7560 |
| order@2345 | 40 | 6768 | 6384-7168 | 11.6 % | 11.6 % | 2.1 % | 7664 / 12.9 % | 7072 |
| pred@2345 | 30 | 7992 | 7600-8592 | 12.4 % | 12.0 % | 3.2 % | 8976 / 15.5 % | 8504 |

`@ORDER` is the host CPUs vCPUs 0-3 were pinned to (`2345` = boot.sh's default). gate p50s in boot order: 7168 7424 7056 7040 7024 6976 7024 7168 7088 7120 7024 6976 7072 6992 7072 7120 7120 7008 7248 7008 | 7040 7072 7040 6928 7104 6976 6944 7488 6992 7232 6960 6960 7040 7040 7056 7136 7008 6976 7040 6960. Everything else is in `raw/tables.md`.

The other lines of the gate image all pass as in the 40-boot campaign (`raw/boots.tsv` has 2 AS passive and own-context p50s per boot).

## By placement (single-measurement images with the placement log)

| image@order | client vCPU | host CPU | boots | p50 median | min-max | spread | worst 10-window | CV |
|---|---|---|---|---|---|---|---|---|
| place@2345 | 0 | 2 | 6 | 7208 | 6800-7312 | 7.1 % | - | 2.6 % |
| place@2345 | 1 | 3 | 10 | 7344 | 6928-7712 | 10.7 % | 10.7 % | 3.6 % |
| place@2345 | 2 | 4 | 15 | 7232 | 6528-7520 | 13.7 % | 11.6 % | 3.2 % |
| place@2345 | 3 | 5 | 9 | 7072 | 7040-7392 | 5.0 % | - | 2.0 % |
| depth@2345 | 1 | 3 | 12 | 7176 | 7120-7584 | 6.5 % | 6.5 % | 2.3 % |
| depth@2345 | 2 | 4 | 13 | 7248 | 6688-7488 | 11.0 % | 11.0 % | 3.0 % |
| depth@2345 | 3 | 5 | 12 | 7160 | 7056-7424 | 5.1 % | 4.7 % | 1.5 % |
| depth@3452 | 1 | 4 | 11 | 7376 | 6960-7504 | 7.4 % | 7.5 % | 2.8 % |
| depth@3452 | 2 | 5 | 18 | 6968 | 6640-7200 | 8.0 % | 8.0 % | 2.1 % |
| depth@4523 | 1 | 5 | 14 | 7440 | 6960-7552 | 8.0 % | 8.0 % | 3.2 % |
| depth@4523 | 2 | 2 | 14 | 7120 | 6816-7392 | 8.1 % | 8.1 % | 2.1 % |
| depth@4523 | 3 | 3 | 11 | 6976 | 6592-7232 | 9.2 % | 9.2 % | 2.5 % |
| depth@5234 | 1 | 2 | 12 | 7016 | 6832-7632 | 11.4 % | 11.4 % | 4.4 % |
| depth@5234 | 2 | 3 | 13 | 7216 | 7008-7360 | 4.9 % | 4.9 % | 1.4 % |
| depth@5234 | 3 | 4 | 14 | 7080 | 6656-7328 | 9.5 % | 9.5 % | 2.9 % |
| order@2345 | 1 | 3 | 12 | 6768 | 6496-7168 | 9.9 % | 9.9 % | 2.3 % |
| pred@2345 | 2 | 4 | 10 | 7864 | 7824-8128 | 3.9 % | 3.9 % | 1.4 % |
| order@2345 | 2 | 4 | 15 | 6864 | 6768-6928 | 2.3 % | 2.3 % | 0.9 % |
| order@2345 | 3 | 5 | 11 | 6720 | 6384-6992 | 9.0 % | 9.1 % | 2.4 % |

(Groups of < 9 boots are in `raw/bytable.md`.)

| image@order | variance share of client vCPU | SD within one placement | SD all boots |
|---|---|---|---|
| place@2345 | 17 % | 3.03 % | 3.20 % |
| depth@2345 | 5 % | 2.37 % | 2.34 % |
| depth@3452 | 52 % | 2.40 % | 3.32 % |
| depth@4523 | 46 % | 2.66 % | 3.46 % |
| depth@5234 | 14 % | 3.13 % | 3.24 % |
| order@2345 | 23 % | 1.89 % | 2.07 % |
| pred@2345 | 35 % | 2.72 % | 3.20 % |

Depth boots of all four orders, each p50 over its order's median:
- by client vCPU: 0 −2.7 % (n = 8), 1 +2.0 % (49), 2 +0.4 % (59), 3 −1.3 % (46);
- by client host CPU: 2 −0.2 % (39), 3 −0.3 % (38), 4 +0.5 % (40), 5 +0.9 % (45).

## Three measurements per boot

| image | boots | ICC | per-boot SD | per-measurement SD | measurement 1 / 2 / 3: median, CV | median of 3 per boot: spread, CV | r(m1,m2) / r(m1,m3) / r(m2,m3) |
|---|---|---|---|---|---|---|---|
| rep (gate kernel) | 30 | 0.22 | 1.31 % | 2.46 % | 7112 1.3 % / 7120 2.9 % / 7064 3.5 % | 9.0 %, 2.5 % | +0.17 / −0.07 / +0.53 |
| rep-place | 30 | 0.63 | 2.84 % | 2.19 % | 7184 3.1 % / 7208 3.6 % / 7248 4.1 % | 9.3 %, 3.2 % | +0.56 / +0.51 / +0.79 |

In `rep-place` the client's CPU per measurement, per boot: 203 123 132 201 320 200 213 321 120 132 203 002 121 132 032 122 300 301 230 223 212 321 213 313 312 132 131 200 212 313. Pairs in a boot with the same client CPU differ by 2.50 % on average (n = 12), pairs with different ones by 2.16 % (n = 78).

## Rounds within a boot, and the lock's order

- Within-boot SD of the 20 round p50s, median per image: base 1.7 %, depth 2.2-2.7 %, order 1.9 %, long 1.0 %. The SD of the boots' round means is 1.5 %, 2.3-3.3 %, 1.8 % and 1.7 % respectively.
- **`order`** (per CPU per round, the gap between its consecutive tickets, plus every CPU's last ticket at each round's start):
  - The busy CPUs' gaps are 4 in 97.3 % of turns and ≤ 3 in 2.7 % (a turn while the client's CPU wasn't queued); ≥ 5 (an overtaking) a median of once per CPU per round, at most 6.
  - From the snapshots (66 % of rounds decodable), the busy CPUs' cyclic direction changes between rounds within a boot.
  - Round p50 by direction, against its boot's mean: busy {0,1,3}: 013 −0.50 % (72 rounds) vs 031 +0.48 % (95); {0,1,2}: 012 −0.46 % (96) vs 021 +0.48 % (68); {0,2,3}: 023 +0.07 % (94) vs 032 +0.24 % (77).
- **`pred`** (per CPU per round, the CPU that held the lock before it; one extra line that only holders touch, which makes this image ~13 % slower):
  - The dominant predecessor's share per CPU per round has median 0.57 (0.38-0.93). The handoff cycle is reshuffled within every round.
  - r(round p50, that share) within boots: median −0.23, range −0.55 to +0.45.

## Host side

r with the contended p50, within each variant:

| variant | ruler lo | ruler hi | host freq at end | pkg temp mean | pkg temp at start | host IRQs 2-5 | boot index |
|---|---|---|---|---|---|---|---|
| gate@2345 | −0.12 | +0.12 | −0.01 | −0.12 | −0.19 | +0.58 | −0.21 |
| depth@2345 | −0.21 | +0.04 | +0.18 | +0.04 | −0.08 | +0.51 | −0.19 |
| depth@3452 | −0.05 | +0.01 | −0.18 | −0.11 | −0.13 | +0.54 | +0.01 |
| depth@4523 | +0.03 | −0.07 | +0.18 | −0.04 | +0.00 | +0.25 | −0.39 |
| depth@5234 | −0.11 | +0.15 | +0.02 | −0.39 | −0.05 | +0.71 | +0.07 |
| long@2345 | +0.06 | +0.10 | −0.08 | +0.03 | +0.11 | +0.72 | −0.16 |
| place@2345 | −0.01 | −0.25 | −0.17 | +0.23 | +0.00 | +0.44 | −0.02 |

- The IRQ column is all LOC (host timer). RES, CAL and TLB on CPUs 2-5 are flat (gate: 4, 44 and 3 per boot) and uncorrelated.
- LOC ticks per cycle of p50: gate 0.121, depth 0.107, place 0.076, long 0.253. Expected from the contended phase lasting longer (4 CPUs × 1 000 Hz × calls ÷ 4.15 GHz): 0.096, and 0.289 for long.
- Host frequency is from `scaling_cur_freq` every 0.2 s. The contended phase is only ~0.2 s, so "at end" is one sample. The ruler is the better clock figure and it doesn't track either.
- Not tested (needs root): host physical placement of the RAM file's pages (`/proc/*/pagemap` PFNs are hidden), KVM's EPT/VMCS pages, uncore frequency.

## Method

- **Tree.** `~/joist-ct3` on the build VM, a clean copy of the 0018 tree.
  - A gate build of it untouched (`iso/pristine.iso`) has kernel `.text` `3d2648c201931ae1`. The light images here (gate, base, long, rep, and gate2-4 rebuilt after every edit) have the same.
  - Pristine copies of the edited files: `tools/orig/`; edited ones: `tools/work/`; the diff: `tools/joist-place0018.patch`.
  - contend2's `joist-ct2.patch` applied with offsets only. On top of it:
    - `run.c`, under `RUN_LOCK_DEPTH` or the new `RUN_PLACE` — a placement log. From op 5 (benchroot, just before it starts the busy threads) to op 0 (the client, as it starts measuring), under the lock, it records:
      - every entry of a thread on a CPU other than the one it last ran on;
      - every idle entry;
      - every poke mask from `joist_sched_pokes`, with the in-flight mask;
      - benchroot's marks (op 4).
      It is printed at op 1. While the measurement runs, its only cost is a load of `pl_on` in `enter` and in `poke_others`. `RUN_PLACE` is this log without contend2's per-acquisition counting.
    - `RUN_LOCK_ORDER`: the counts are each CPU's gap to its own previous ticket, plus a snapshot of every CPU's last ticket at each round start (op 3, on the client's CPU).
    - `RUN_LOCK_PRED`: the counts are the previous holder's CPU (a line written only under the lock).
    - `benchroot.c`: the marks (`BENCH_LOCK_DEPTH` only) and `BENCH_REPEAT` (the contended measurement k times in one boot, a new client and server each time, the busy threads running throughout).
- **Images** (`tools/build*.sh`; all the bench-gate entry `ROOT_TASK=benchroot MODULES=ipcbench`; SHA-256 in `raw/images.sha256`):

  | image | kernel flags | user flags | kernel .text |
  |---|---|---|---|
  | gate | - | - | 3d2648c2 (= pristine) |
  | base | - | BENCH_ROUNDS | 3d2648c2 |
  | long | - | BENCH_ROUNDS, BENCH_CALLS=15000 | 3d2648c2 |
  | rep | - | BENCH_REPEAT=3 | 3d2648c2 |
  | place | RUN_PLACE | BENCH_LOCK_DEPTH | 0f1bc05a |
  | rep-place | RUN_PLACE | BENCH_LOCK_DEPTH, BENCH_REPEAT=3 | 0f1bc05a |
  | depth | RUN_LOCK_DEPTH | BENCH_ROUNDS, BENCH_LOCK_DEPTH | 5409f104 |
  | order | RUN_LOCK_DEPTH, RUN_LOCK_ORDER | BENCH_ROUNDS, BENCH_LOCK_DEPTH | 1606c179 |
  | pred | RUN_LOCK_DEPTH, RUN_LOCK_PRED | BENCH_ROUNDS, BENCH_LOCK_DEPTH | 58b6248a |

- **Boots.** `tools/runiso.sh` is `boot.sh.current`'s remote half, run on the P53 directly:
  - the same QEMU line;
  - the same zeroed `/dev/shm/joist-test-ram`;
  - the same build-VM narrowing to 0,1,6,7 and restore to 0,1,6-11;
  - `flock /srv/joist/test/lock`.
  It takes an optional vCPU→host CPU order (boot.sh's `P53_CPUS`), always a permutation of 2-5, and records every QEMU thread's affinity and last host CPU at 1 s.
- **Campaigns.** `tools/campaign.sh` is contend2's, for `IMG@ORDER` variants: interleaved rounds with a rotating start, `check.sh` and the governor before every boot, the 2 % build-VM gate before and over every boot, `/proc/interrupts` before and after, a 0.2 s sampler (frequency of 2-5, coretemp).

  | campaign | variants | boots each | time |
  |---|---|---|---|
  | 1 | gate, depth, base @2345 | 20 | 13:11-13:19 |
  | 2 | depth @3452, @4523, @5234, @2345; gate @2345 | to 40 | 13:19-13:41 |
  | 3 | place, order, long @2345 | 40 | 13:44-14:01 |
  | 4 | rep, rep-place, pred @2345 | 30 | 14:02-14:15 |

  - Campaign 2 was restarted once at 13:19 after a mistyped variant name. One boot from the first try (`depth@3452-1`) is kept; it passed the gate like the rest.
  - Builds ran only between campaigns.
- **Host settings.**
  - `tools/check.sh` is contend's, except that the build VM's resting pinning is now 0,1,6-11.
  - It passed before every boot and after every campaign (`raw/check-*.txt`): governor `performance` on 2-5, `default_smp_affinity` c3, only the known per-CPU NVMe queue IRQs on 2-5/8-11, build VM QEMU pid 9937.
- **Analysis.**
  - `tools/analyze.py` gives `raw/boots.json` and `raw/boots.tsv`, one row per boot.
  - `tools/stats.py` gives `raw/tables.md`; `tools/bytable.py` gives `raw/bytable.md`.
  - `tools/criterion_sim.py` is the window simulation.
  - Units are ipcbench's own cycles.

## Files

- `raw/IMG@ORDER-N.log`: the serial log with the runner's header and pins. Each has `.freq`, `.irq-before` and `.irq-after` beside it.
- Also in `raw/`: `campaign.log`, `campaign*.out`, `check-*.txt`, `images.sha256`, `smoke/` (the smoke boots), `boots.json`, `boots.tsv`, `tables.md`, `bytable.md`.
- `iso/`: every image booted, plus `pristine.iso`.
- `tools/`: the patch, `orig/`, `work/`, `build*.sh`, `runiso.sh`, `campaign.sh`, `check.sh` and the analysis scripts.
