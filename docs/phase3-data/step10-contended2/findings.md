# The contended line after 8241b56: saturation or not? (2026-10-16)

Measurements only. I changed nothing outside my copy `~/joist-ct2` on the build VM, committed nothing and touched no system setting.

## Answer in short

1. **The hypothesis does not hold for today's kernel. The lock is still a full convoy.**
   - A TSC-free counter (`RUN_LOCK_DEPTH`) shows the client finding 3 tickets ahead in 98.7-99.9 % of its acquisitions, in every round of every boot. That is the holder plus both other busy CPUs. The client's mean tickets ahead per round is 2.98-3.00.
   - The stats build agrees (99.8 %).
   - The phase-dependent regime you describe does exist, but only when the busy CPUs spend ≥ 1 000 TSC counts in user mode between calls. There the client's tickets ahead vary per round (1.7-2.6). The round p50 follows them: r = +0.9 within boots, +0.97 across boots. The boot-to-boot spread **peaks at N = 2000 (25.5 %)**, where the client finds 1 or 2 ahead about half the time each. It falls away on both sides: N = 3 000 gives 18.9 %, N = 10 000 gives 0.2 %. On the saturated side (N ≤ 600) it is flat at 5.7-8.4 %.
   - The sharp change in the queue the client sees comes between N = 600 and N = 3 000. The busy CPUs' share of lock time (stats build) falls only from 39 % to 25 % over that range. Total lock utilisation, client included, stays at 69-72 % throughout, so it is no indicator.
   - **Consequence: shortening the busy CPUs' path further would move the line toward the peak, not away from it.**
2. **The distribution has one peak. Rounds differ by a shift of the whole distribution, not by weights moving between fixed modes.**
   - Every boot's pooled 128-cycle histogram has a single mode, holding 50-76 % of samples within ±2 buckets.
   - Round to round, the round's p10 and p90 move with its p50: r = +0.56 to +0.98 across the 30 `base`, `ts` and `ts-depth` boots.
3. **What does move the N = 0 line. Three parts, none of them the queue:**
   - **a. Placement, set at each boot.** Global scheduling puts the client on a different vCPU each boot.
     - In the TSC-free builds with N ≤ 600 (50 boots), the client's vCPU explains **47 % of the boot-to-boot variance**: vCPU 3 is −1.2 %, vCPUs 1 and 2 are +1.4 %.
     - With fewer busy CPUs placement is almost everything. With 1 busy CPU each (client, busy) core pair has its own level, repeatable within < 1 %: client 1 / busy 0 gives 3 512, 3 520, 3 520, 3 528. The levels run from 3 096 to 4 096 (32 %).
     - With 2 busy CPUs: busy on vCPUs {0,1} gives 4 976; on {0,2} it gives 5 568.
   - **b. A time-locked host transient.** 45-65 ms after the busy threads start, the core clock dips about 3 % (ruler 7 265 → 7 470 counts), and batch medians rise 3-6 %.
     - It happens at the same *time* in every variant, so at different *round numbers*: rounds 6-8 at N = 0, round ~14 at N = 3 000, rounds 13-16 with 1 busy CPU.
     - At N = 0 it makes one of rounds 6-8 the worst round in 9 of 10 boots.
     - Letting the busy threads run 300 ms or 1 s before the measurement removes it; the series is then flat. The median's spread across boots stays (8.5 %, 10.6 %), so this transient is not what moves the median. It moves the worst round and p99.
   - **c. A residual I could not attribute.**
     - Round p50s within a boot have SD 160-220 cycles (2.3-3 %). There is also a per-boot offset; after removing placement, the boot-to-boot SD is 1.7 %.
     - It does not follow the client's tickets ahead (only 0.1-1.3 % of its acquisitions find fewer than 3), the core clock (r −0.3 to +0.1 within boots, inconsistent sign across boots), or host interrupts.
     - The TSC-free depth builds, which add one load and two increments per acquisition, are ~1.6 % slower and spread only 3.4-3.6 % (20 boots). The plain light builds' spread is set mostly by a few *fast* boots (6 464-6 816). Why the counters remove those fast boots I don't know. I haven't tested it.
4. **Against the criterion.** The bench-gate image (`gate`, `gate-r2`, 20 boots in two campaigns) gave 6 864-7 328 cycles: 6.6 %, CV 1.7 %. None of its 11 consecutive 10-boot windows is under 5 % (5.9-6.6 %). Drawing 10 of the 50 N = 0 light boots (gate, base, ts) at random gives under 5 % in 21 % of draws. p99 spreads are 6-9 %, inside the 20 % limit.
5. **For the other session (observations, not recommendations):**
   - At N = 0 there is no phase effect to remove. The convoy is the *stable* side.
   - About half the variance is which vCPU the client lands on. The ABI has no CPU affinity (`user/lib/joist.h`), so the benchmark can't pin it today.
   - A ≥ 300 ms lead for the busy threads is a sound benchmark change for the worst round and p99, because it removes a host clock transient. On its own it does not bring the median under 5 %.

Item 3-4 budget: everything asked was done, plus extra spin points (N = 100, 600, 2 000), TSC-free depth builds and a per-batch time series. Boots ran from 15:52 to 16:46 (local time), about 54 minutes. There were 370 kept boots and 1 discard: at 16:14 I ran a grep on the build VM, which pushed it over the 2 % gate, and the boot was repeated.

## Item 3 and 4: the saturation sweep (10 boots each)

`N` = TSC counts each busy CPU spins in user mode between its calls. *Tickets ahead* counts the holder: 3 = behind all three busy CPUs. Lock figures come from the stats build (`stats-*`) or the TSC-free depth build (`depth-*`), both booted interleaved with the light image. All p50s are of the light image unless the column says otherwise.

| variant | boots | contended p50 median | min-max | spread across boots | CV | worst round p50 median / max | within-boot round spread (median) | p99 median / spread | lock utilisation: all / busy CPUs' holds (stats build) | client tickets ahead 0/1/2/3 % (stats build) | client tickets ahead 0/1/2/3 % (depth build, no TSC) | stats build p50 spread |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 3 busy, N=0 (base) | 10 | 7008 | 6784-7344 | 8.0 % | 2.0 % | 7344 / 7792 | 9.0 % | 8064 / 9.1 % | 71.0 % / 40.2 % | 0.0/0.0/0.2/99.8 | 0.0/0.0/0.5/99.5 | 5.2 % |
| 3 busy, N=100 | 10 | 6992 | 6880-7280 | 5.7 % | 1.6 % | 7336 / 7552 | 8.4 % | 8048 / 4.8 % | - | - | 0.0/0.0/2.0/97.9 | - |
| 3 busy, N=300 | 10 | 6896 | 6672-7248 | 8.4 % | 2.5 % | 7320 / 7616 | 9.7 % | 7944 / 6.2 % | 68.8 % / 38.5 % | 0.0/0.0/0.3/99.7 | 0.0/0.0/1.3/98.6 | 9.3 % |
| 3 busy, N=600 | 10 | 7024 | 6768-7240 | 6.7 % | 2.2 % | 7384 / 7544 | 9.0 % | 8016 / 7.2 % | 71.8 % / 39.4 % | 0.0/0.1/0.2/99.7 | 0.0/0.1/1.0/98.9 | 8.9 % |
| 3 busy, N=1000 | 10 | 5848 | 5200-6040 | 14.4 % | 4.5 % | 6512 / 6752 | 17.4 % | 7608 / 14.1 % | 70.4 % / 39.3 % | 0.0/0.1/36.0/63.9 | 0.1/5.2/76.4/16.4 | 13.9 % |
| 3 busy, N=2000 | 10 | 4488 | 3568-4712 | 25.5 % | 8.9 % | 4678 / 4876 | 8.5 % | 5026 / 12.7 % | 71.9 % / 29.7 % | 0.1/49.9/49.8/0.1 | 1.2/56.9/41.4/0.1 | 7.7 % |
| 3 busy, N=3000 | 10 | 3590 | 3232-3912 | 18.9 % | 6.7 % | 3952 / 4152 | 20.2 % | 4516 / 11.7 % | 71.8 % / 24.5 % | 0.6/98.7/0.6/0.1 | 21.2/73.7/4.0/0.1 | 8.6 % |
| 3 busy, N=10000 | 10 | 1640 | 1636-1640 | 0.2 % | 0.1 % | 1656 / 1708 | 1.2 % | 3982 / 18.2 % | 69.4 % / 10.5 % | 72.8/26.9/0.2/0.0 | - | 3.1 % |
| 2 busy, N=0 | 10 | 4840 | 4752-5360 | 12.6 % | 5.1 % | 5544 / 5680 | 9.5 % | 5984 / 11.8 % | 73.9 % / 36.4 % | 0.0/0.1/99.9/0.0 | 0.0/0.6/99.4/0.0 | 6.9 % |
| 1 busy, N=0 | 10 | 3376 | 3144-4088 | 28.0 % | 10.5 % | 3596 / 4312 | 7.0 % | 3728 / 33.3 % | 71.6 % / 24.8 % | 0.1/99.9/0.0/0.0 | 0.1/99.9/0.0/0.0 | 8.6 % |

The extra points, the controls and the time-series images:

| variant | boots | contended p50 median | min-max | spread across boots | CV | worst round p50 median / max | within-boot round spread (median) | p99 median / spread | lock utilisation: all / busy CPUs' holds (stats build) | client tickets ahead 0/1/2/3 % (stats build) | client tickets ahead 0/1/2/3 % (depth build, no TSC) | stats build p50 spread |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gate (bench-gate image, no flags) | 10 | 7032 | 6864-7312 | 6.4 % | 1.8 % | 7368 / 7856 | - | 8080 / 7.7 % | - | - | - | - |
| gate-r2 (same ISO, campaign 2) | 10 | 7032 | 6912-7328 | 5.9 % | 1.6 % | 7408 / 7760 | - | 7992 / 6.0 % | - | - | - | - |
| base-r2 (same ISO as base, campaign 2) | 10 | 6992 | 6784-7312 | 7.6 % | 1.8 % | 7416 / 7744 | 8.8 % | 8048 / 7.8 % | - | - | - | - |
| ts (N=0, + series) | 10 | 6992 | 6800-7344 | 7.8 % | 2.5 % | 7328 / 7920 | 7.8 % | 7952 / 6.2 % | - | - | 0.0/0.0/1.0/99.0 | - |
| ts-lead300 | 10 | 6992 | 6736-7328 | 8.5 % | 2.4 % | 7264 / 7728 | 8.1 % | 7936 / 8.9 % | - | - | - | - |
| ts-lead1000 | 10 | 6960 | 6464-7200 | 10.6 % | 3.8 % | 7288 / 7728 | 9.3 % | 7824 / 12.3 % | - | - | - | - |
| depth (N=0, TSC-free counters) | 10 | 7120 | 7008-7248 | 3.4 % | 1.1 % | 7464 / 7680 | 10.2 % | 8264 / 9.7 % | - | - | 0.0/0.0/0.5/99.5 | - |
| ts-depth | 10 | 7112 | 7056-7312 | 3.6 % | 1.0 % | 7784 / 7920 | 12.4 % | 8248 / 7.4 % | - | - | 0.0/0.0/1.0/99.0 | - |
| ts-s3000 | 10 | 3512 | 3216-3844 | 17.9 % | 5.7 % | 3990 / 4084 | 20.9 % | 4522 / 10.6 % | - | - | - | - |
| ts-b1 | 10 | 3344 | 3120-4072 | 28.5 % | 9.1 % | 3600 / 4392 | 8.7 % | 3748 / 30.1 % | - | - | 0.4/99.6/0.0/0.0 | - |
| depth-b1 | 10 | 3252 | 3088-3528 | 13.5 % | 5.1 % | 3520 / 3776 | 9.3 % | 3660 / 7.7 % | - | - | 0.1/99.9/0.0/0.0 | - |
| depth-b2 | 10 | 5384 | 4960-5632 | 12.5 % | 5.2 % | 5744 / 6000 | 10.2 % | 6248 / 15.6 % | - | - | 0.0/0.6/99.4/0.0 | - |

Per-boot contended p50, then the worst round's p50, every image in boot order:

| image | p50, boots 1-10 | worst round p50, boots 1-10 |
|---|---|---|
| gate | 6976 7040 7104 7312 7040 7232 7008 7024 6960 6864 | 7184 7200 7424 7856 7408 7632 7328 7440 7296 7056 |
| gate-r2 | 7328 7104 7040 6992 6976 7040 6944 7024 7056 6912 | 7760 7712 7728 7360 7392 7312 7376 7424 7440 7072 |
| base | 7040 7344 6928 7008 6976 6784 7008 7024 7120 6896 | 7456 7776 7312 7376 7296 7248 7792 7488 7264 7280 |
| base-r2 | 7312 7008 6992 6992 6912 7072 6784 6976 6960 7008 | 7744 7536 7472 7712 7264 7344 7200 7360 7344 7584 |
| ts | 7056 6912 6928 7104 6928 7264 6800 6816 7344 7104 | 7200 7312 7344 7920 7136 7552 7168 7232 7696 7472 |
| ts-lead300 | 6816 7024 6832 6736 7040 6944 6960 7184 7088 7328 | 7184 7264 7312 6928 7136 7264 7216 7728 7440 7536 |
| ts-lead1000 | 6464 6992 6864 7104 6928 7184 6624 7168 7200 6544 | 7024 7280 6976 7296 7344 7600 7040 7424 7728 6768 |
| depth | 7136 7104 7024 7104 7232 7008 7168 7168 7056 7248 | 7360 7584 7632 7440 7440 7488 7344 7424 7600 7680 |
| ts-depth | 7104 7312 7056 7056 7120 7120 7056 7120 7072 7184 | 7920 7824 7568 7376 7792 7776 7520 7280 7888 7824 |
| s100 | 6928 6880 7280 6896 6992 7072 6992 7008 6928 7120 | 7232 7424 7424 7232 7408 7328 7248 7344 7232 7552 |
| s300 | 6752 6896 6960 6704 6672 7056 6896 6880 7120 7248 | 7216 7584 7280 7120 7088 7360 7616 7120 7568 7408 |
| s600 | 7040 6992 7216 7024 7136 7240 6864 6768 7024 6800 | 7392 7472 7456 7296 7376 7544 7104 7136 7472 7104 |
| s1000 | 5680 5696 6000 5376 5936 5904 5808 5200 5888 6040 | 6512 6160 6560 5888 6736 6752 6376 6288 6512 6704 |
| s2000 | 4520 4712 4680 3568 4456 4452 3648 4448 4588 4576 | 4652 4864 4824 4360 4704 4620 4440 4596 4876 4736 |
| s3000 | 3752 3912 3292 3232 3240 3584 3648 3724 3596 3248 | 4056 4152 3912 3884 3600 3928 4108 4004 3976 3496 |
| ts-s3000 | 3216 3756 3844 3652 3652 3500 3524 3340 3388 3264 | 3648 4056 4036 4024 4068 3956 4084 3808 3808 3520 |
| s10000 | 1640 1640 1640 1640 1640 1640 1636 1640 1640 1640 | 1640 1672 1648 1708 1668 1640 1640 1664 1668 1640 |
| b2 | 5344 4832 4800 4784 4816 5360 4752 5264 5328 4848 | 5680 5344 5664 5280 5040 5664 5584 5504 5584 4944 |
| b1 | 3448 3200 4088 4072 3152 3304 3304 4024 3448 3144 | 3760 3472 4312 4280 3464 3504 3504 4224 3688 3208 |
| ts-b1 | 3192 3296 3120 4072 4000 3288 3392 3392 3448 3192 | 3208 3536 3536 4392 4184 3488 3696 3664 3736 3408 |
| depth-b2 | 5632 5584 5552 5600 5248 5120 4976 4960 4976 5520 | 5888 5824 6000 5840 5600 5696 5152 5200 5248 5792 |
| depth-b1 | 3512 3088 3280 3168 3400 3448 3528 3096 3224 3096 | 3664 3448 3528 3472 3608 3696 3776 3416 3456 3512 |
| ts-depth-b1 | 4096 3520 3104 3112 3192 3096 3528 3280 3512 3520 | 4464 3760 3432 3424 3504 3296 3672 3464 3784 3776 |
| stats | 9312 9120 9104 9440 9504 9168 9312 9024 9504 9136 | 9552 9248 9488 9616 9552 9264 9536 9136 9680 9264 |
| stats-s300 | 9216 8800 9408 8976 9664 9344 9440 9136 9280 9328 | 9520 9504 9536 9152 9888 9472 9536 9200 9392 9440 |
| stats-s600 | 8592 9408 9152 8960 9280 8832 9360 9312 9088 9120 | 8672 9568 9312 9056 9488 8976 9632 9584 9392 9280 |
| stats-s1000 | 8016 7920 7872 8992 7968 8944 8048 8048 7904 8784 | 8304 8048 8160 9536 8432 9104 8960 9184 8784 9088 |
| stats-s2000 | 5960 6368 6176 6032 6064 6000 6272 5984 5936 6400 | 6104 6480 6464 6160 6112 6048 6400 6064 6128 6512 |
| stats-s3000 | 5136 5152 5192 5320 5208 5120 4896 5032 5064 5336 | 5184 5208 5232 5456 5296 5184 4912 5064 5112 5384 |
| stats-s10000 | 2640 2592 2600 2624 2584 2600 2664 2608 2600 2592 | 2664 2648 2736 2704 2680 2712 2728 2704 2616 2712 |
| stats-b2 | 7040 6832 7168 7040 6976 6976 6928 6896 6896 6688 | 7152 6928 7232 7184 7136 7024 6976 7120 6976 6752 |
| stats-b1 | 5272 5008 5008 4992 4992 4848 5280 4944 5008 4944 | 5376 5088 5040 5088 5040 4864 5456 4960 5136 4984 |

**Reading.**
- With N ≤ 600 the client is behind all three busy CPUs and nothing about the queue changes from round to round. The spread is 5.7-8.4 %, about the bench-gate image's.
- From N = 1 000 to 3 000, how far back the client queues changes from round to round and boot to boot. The p50 follows it closely: r(round p50, round mean tickets ahead) is +0.67 to +0.99 within each boot, and +0.97 across boots for both depth-s1000 and depth-s3000.
- This is the hypothesis's mechanism. It is strongest where the client's queue length is split about evenly between two values (N = 2 000: 57 % one ahead, 41 % two).
- Where the queue settles is largely set by placement, though, not by chance.
  - At N = 2 000 rounds within a boot spread only 8.5 % (median), against 25.5 % across boots. The depth build's boots sort by the client's vCPU: vCPU 0 gives 3 744, vCPU 3 gives 4 424-4 480, vCPU 1 gives 4 576-4 768 (`raw/tables.md`, depth-s2000).
  - At N = 1 000 and 3 000 both kinds of spread are large: rounds 17-21 %, boots 14-19 %.
- The stats build saturates more easily: its longer holds keep the client at 3 ahead up to N = 600 and at 2-3 at N = 1 000. Its spreads are lower (7.7 % at N = 2 000).

## Item 1: per round, histograms (light build `base`, N = 0, 3 busy)

| boot | p50 | p99 | min | worst round p50 | round p50s min-med-max | round spread | round SD | r(round p50, round p10 / p90) | modes of the pooled histogram (cycles, share within ±2 buckets) | host IRQs CPUs 2-5 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 7040 | 8160 | 4889 | 7456 | 6688-7032-7456 | 10.9 % | 158 | +0.77 / +0.83 | 6976 (68.8 %) | 2453 |
| 2 | 7344 | 8432 | 4894 | 7776 | 7136-7336-7776 | 8.7 % | 146 | +0.88 / +0.93 | 7232 (69.1 %) | 2462 |
| 3 | 6928 | 7808 | 4542 | 7312 | 6624-6888-7312 | 10.0 % | 206 | +0.79 / +0.94 | 6720 (57.6 %) | 2398 |
| 4 | 7008 | 7856 | 5128 | 7376 | 6880-7000-7376 | 7.1 % | 117 | +0.93 / +0.91 | 6976 (76.6 %) | 2438 |
| 5 | 6976 | 7872 | 4953 | 7296 | 6720-6968-7296 | 8.3 % | 137 | +0.89 / +0.84 | 6976 (74.0 %) | 2505 |
| 6 | 6784 | 8000 | 4897 | 7248 | 6624-6776-7248 | 9.2 % | 157 | +0.84 / +0.89 | 6720 (66.5 %) | 2411 |
| 7 | 7008 | 8208 | 4701 | 7792 | 6944-7008-7792 | 12.1 % | 237 | +0.98 / +0.92 | 6976 (64.7 %) | 2474 |
| 8 | 7024 | 8128 | 5259 | 7488 | 6816-7024-7488 | 9.6 % | 151 | +0.92 / +0.93 | 6976 (70.2 %) | 2469 |
| 9 | 7120 | 8320 | 5102 | 7264 | 6896-7112-7264 | 5.2 % | 108 | +0.78 / +0.83 | 7104 (71.8 %) | 2455 |
| 10 | 6896 | 7696 | 4416 | 7280 | 6720-6888-7280 | 8.1 % | 136 | +0.83 / +0.76 | 6848 (72.6 %) | 2446 |

Round p50s, all boots: 200 rounds, 6624-7792; SD within a boot 160, SD of the boots' round means 141 cycles.

Round p50s in order, per boot:

| boot | rounds 1-20 |
|---|---|
| 1 | 6960 6928 7456 6688 6880 6912 7248 7008 7056 7056 7088 7072 7024 7168 7040 6848 7008 7136 7024 7216 |
| 2 | 7584 7168 7296 7136 7232 7776 7344 7360 7264 7552 7248 7456 7328 7424 7312 7280 7344 7392 7248 7344 |
| 3 | 7152 6976 6624 7152 6688 6656 7312 7280 6704 7072 7008 6960 6848 6688 6800 6688 6736 6832 6928 6960 |
| 4 | 6976 6912 7136 7040 6992 7376 7248 7024 6960 7008 7040 7008 7024 6960 7040 6960 6912 6960 6880 6880 |
| 5 | 6752 6720 6960 6784 6912 7184 7296 6880 6960 7056 6912 6944 6976 7056 7056 7024 6864 7120 7008 6976 |
| 6 | 6960 6640 6784 6784 6704 6704 7040 7248 6960 6720 6832 6624 6976 6768 6768 6656 6928 6704 6720 6960 |
| 7 | 7456 7440 7392 7408 7248 7792 6976 7104 6944 6944 7040 7008 6944 6976 6944 6960 6944 7232 7008 6976 |
| 8 | 6944 6832 7024 7152 6816 7280 7488 7120 7024 6992 6944 7120 7184 7024 6960 7056 6992 6960 6992 7200 |
| 9 | 7024 7232 6912 6896 7008 6960 7136 7264 7120 7072 7216 7232 7168 7184 7200 7008 7104 7088 7136 6992 |
| 10 | 6752 6976 6736 6720 6800 6736 7120 7280 6864 6992 6976 6864 6992 6880 6976 6864 6960 6928 6896 6768 |

One boot's histogram (base-7, the most spread one: rounds 1-6 vs 7-20), 128-cycle buckets, ‰ of the round, from 6 144 to 8 448. One peak per round. Rounds 1-6 sit ~300 cycles higher than 7-20, the whole shape moved:

## Every line, per image: p50 median / min-max / spread (p99 likewise)

**gate** (10 boots)

| line | p50 | p99 |
|---|---|---|
| null call | 412 / 408-412 / 1.0 % | 424 / 420-432 / 2.8 % |
| 2 AS passive | 1632 / 1628-1632 / 0.2 % | 1668 / 1640-1704 / 3.8 % |
| 1 AS passive | 1176 / 1172-1176 / 0.3 % | 1188 / 1180-1204 / 2.0 % |
| 2 AS own ctx | 14016 / 13920-14144 / 1.6 % | 20368 / 19616-22208 / 12.7 % |
| 2 AS 4 KiB | 2984 / 2984-2984 / 0.0 % | 3040 / 3000-3056 / 1.8 % |
| contended | 7032 / 6864-7312 / 6.4 % | 8080 / 7760-8384 / 7.7 % |

**base** (10 boots)

| line | p50 | p99 |
|---|---|---|
| null call | 412 / 408-412 / 1.0 % | 430 / 424-432 / 1.9 % |
| 2 AS passive | 1628 / 1628-1628 / 0.0 % | 1672 / 1668-1708 / 2.4 % |
| 1 AS passive | 1172 / 1172-1172 / 0.0 % | 1188 / 1180-1228 / 4.0 % |
| 2 AS own ctx | 13968 / 13856-14080 / 1.6 % | 20112 / 19776-22880 / 15.4 % |
| 2 AS 4 KiB | 2984 / 2984-2984 / 0.0 % | 3004 / 3000-3048 / 1.6 % |
| contended | 7008 / 6784-7344 / 8.0 % | 8064 / 7696-8432 / 9.1 % |

| round | 6144 | 6272 | 6400 | 6528 | 6656 | 6784 | 6912 | 7040 | 7168 | 7296 | 7424 | 7552 | 7680 | 7808 | 7936 | 8064 | 8192 | 8320 | 8448 | round p50 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | 0 | 0 | 0 | 3 | 14 | 41 | 101 | 143 | 161 | 153 | 130 | 100 | 67 | 36 | 17 | 9 | 3 | 3 | 7456 |
| 2 | 0 | 0 | 0 | 0 | 2 | 11 | 36 | 84 | 147 | 207 | 201 | 142 | 79 | 36 | 17 | 11 | 10 | 5 | 3 | 7440 |
| 3 | 0 | 0 | 0 | 1 | 4 | 20 | 62 | 106 | 170 | 211 | 166 | 110 | 57 | 35 | 17 | 12 | 10 | 8 | 3 | 7392 |
| 4 | 0 | 0 | 2 | 6 | 15 | 41 | 73 | 116 | 134 | 130 | 108 | 84 | 85 | 72 | 52 | 40 | 22 | 7 | 3 | 7408 |
| 5 | 0 | 0 | 0 | 0 | 4 | 26 | 108 | 204 | 247 | 195 | 95 | 45 | 24 | 18 | 14 | 7 | 3 | 1 | 0 | 7248 |
| 6 | 0 | 0 | 0 | 0 | 0 | 1 | 4 | 12 | 35 | 76 | 105 | 142 | 166 | 150 | 118 | 83 | 43 | 24 | 13 | 7792 |
| 7 | 1 | 6 | 20 | 64 | 139 | 188 | 156 | 128 | 103 | 80 | 52 | 34 | 13 | 6 | 1 | 1 | 0 | 0 | 0 | 6976 |
| 8 | 0 | 1 | 4 | 19 | 64 | 131 | 198 | 191 | 158 | 92 | 52 | 33 | 20 | 15 | 7 | 3 | 2 | 1 | 0 | 7104 |
| 9 | 1 | 4 | 16 | 67 | 146 | 235 | 217 | 138 | 66 | 35 | 20 | 12 | 13 | 10 | 5 | 4 | 2 | 1 | 0 | 6944 |
| 10 | 0 | 4 | 30 | 81 | 151 | 207 | 189 | 142 | 81 | 48 | 27 | 16 | 8 | 4 | 2 | 0 | 0 | 0 | 0 | 6944 |
| 11 | 0 | 4 | 13 | 51 | 107 | 162 | 174 | 152 | 98 | 57 | 39 | 24 | 20 | 22 | 23 | 17 | 11 | 7 | 2 | 7040 |
| 12 | 0 | 0 | 4 | 29 | 112 | 200 | 214 | 162 | 96 | 60 | 43 | 32 | 19 | 9 | 5 | 1 | 0 | 1 | 0 | 7008 |
| 13 | 0 | 1 | 8 | 50 | 143 | 237 | 240 | 159 | 79 | 42 | 17 | 9 | 4 | 2 | 0 | 0 | 0 | 0 | 0 | 6944 |
| 14 | 0 | 0 | 5 | 43 | 130 | 217 | 220 | 135 | 82 | 64 | 46 | 30 | 12 | 5 | 1 | 0 | 0 | 0 | 0 | 6976 |
| 15 | 0 | 1 | 9 | 50 | 145 | 232 | 227 | 151 | 73 | 44 | 24 | 19 | 11 | 5 | 3 | 1 | 1 | 0 | 0 | 6944 |
| 16 | 0 | 2 | 11 | 53 | 135 | 224 | 240 | 147 | 88 | 49 | 24 | 11 | 6 | 3 | 1 | 0 | 0 | 0 | 0 | 6960 |
| 17 | 0 | 1 | 8 | 47 | 137 | 245 | 230 | 158 | 78 | 46 | 24 | 10 | 7 | 2 | 1 | 0 | 0 | 0 | 0 | 6944 |
| 18 | 0 | 0 | 0 | 5 | 25 | 74 | 140 | 174 | 169 | 129 | 109 | 76 | 43 | 24 | 13 | 7 | 2 | 1 | 0 | 7232 |
| 19 | 0 | 1 | 8 | 37 | 109 | 188 | 232 | 188 | 118 | 64 | 26 | 12 | 6 | 1 | 1 | 1 | 0 | 0 | 0 | 7008 |
| 20 | 0 | 0 | 2 | 28 | 118 | 230 | 265 | 184 | 96 | 35 | 16 | 9 | 5 | 3 | 1 | 0 | 0 | 0 | 0 | 6976 |
| all | 0 | 1 | 7 | 32 | 84 | 144 | 163 | 142 | 113 | 91 | 67 | 49 | 35 | 24 | 16 | 10 | 6 | 3 | 1 | 7008 |

Round p50 relative to its boot's median, mean over boots, by round. The bump at the same *time* lands on different rounds, because rounds are shorter when the line is faster:

| image | boots | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | boots with round 6, 7 or 8 its highest |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| base | 10 | +0.8 | -0.3 | +0.4 | -0.4 | -1.1 | +1.9 | +3.1 | +2.2 | -0.2 | +0.6 | +0.4 | +0.3 | +0.6 | +0.1 | +0.1 | -1.0 | -0.3 | +0.5 | -0.3 | +0.4 | 9 |
| base-r2 | 10 | -0.2 | -0.2 | -0.3 | +0.7 | +1.0 | +2.3 | +3.5 | +1.1 | +0.1 | +0.3 | -0.3 | -0.1 | +0.5 | +0.2 | -0.2 | +1.0 | +1.4 | +0.4 | +0.4 | -0.1 | 7 |
| ts | 10 | +1.6 | -0.3 | +0.5 | +1.2 | -0.1 | +2.8 | +3.2 | +1.3 | -0.8 | -0.4 | -0.4 | -0.1 | -0.4 | +0.1 | +0.2 | -0.6 | +0.4 | +0.3 | -0.3 | -0.4 | 8 |
| ts-lead300 | 10 | -0.9 | -0.1 | +0.3 | +0.1 | +0.4 | -0.3 | +0.5 | +0.2 | +0.1 | -0.0 | +0.6 | +0.6 | -0.2 | +0.4 | -0.6 | -0.4 | -0.9 | +0.2 | +1.1 | +0.7 | 1 |
| ts-lead1000 | 10 | +0.2 | -0.0 | -0.1 | +0.2 | +0.8 | -0.4 | -0.7 | -0.6 | -0.0 | +0.4 | +0.9 | +2.2 | +1.1 | -0.6 | -1.4 | -1.2 | -1.2 | -0.9 | -0.5 | -1.3 | 0 |
| ts-depth | 10 | +1.3 | +0.2 | +1.8 | +1.8 | +1.7 | +5.1 | +0.3 | +0.8 | -0.4 | -0.5 | -0.0 | -0.1 | -0.5 | +0.0 | -1.1 | -1.3 | -0.6 | -1.0 | -0.9 | +0.9 | 7 |
| depth | 10 | +1.8 | -0.6 | +0.5 | +0.9 | +0.6 | +3.2 | +2.0 | -1.3 | -0.5 | -1.4 | -0.7 | -0.5 | -0.3 | -1.4 | -0.6 | -0.7 | -1.3 | -1.0 | +0.3 | +0.4 | 5 |
| s100 | 10 | -0.2 | +0.6 | +0.8 | -0.8 | -0.3 | +2.9 | +2.3 | +1.1 | +0.2 | -0.5 | -0.9 | -1.1 | -0.7 | -0.7 | -0.7 | -0.1 | +1.3 | +0.9 | +0.4 | -0.4 | 6 |
| s300 | 10 | +0.7 | +1.4 | +0.7 | -0.2 | +2.0 | +1.8 | +3.7 | +1.5 | +0.5 | -0.6 | -0.8 | -0.4 | +0.0 | -0.0 | -0.5 | -1.1 | -1.1 | -0.1 | -1.2 | -0.2 | 7 |
| s600 | 10 | -0.7 | +0.7 | +1.1 | -0.1 | +1.0 | +3.0 | +1.7 | -0.6 | -0.3 | -0.1 | -0.5 | -0.7 | +0.0 | +0.1 | -0.6 | -0.5 | -0.5 | -0.8 | -0.2 | -1.5 | 6 |
| s1000 | 10 | -0.6 | -0.4 | -1.9 | +1.5 | +1.5 | +0.9 | +3.2 | +7.1 | +3.5 | -2.4 | -1.2 | -0.9 | -0.6 | +0.1 | -0.8 | -0.8 | -0.3 | +0.5 | +1.3 | +1.3 | 5 |
| s2000 | 10 | +1.0 | +0.4 | +0.6 | -0.4 | -0.9 | -0.0 | +0.7 | +0.5 | +1.4 | +2.3 | +2.5 | +3.8 | +0.6 | -0.2 | -0.6 | -1.4 | -1.0 | -0.2 | -0.6 | -1.0 | 3 |
| s3000 | 10 | +0.1 | +1.1 | +0.2 | +1.4 | -1.7 | +0.3 | -1.3 | -1.5 | -3.4 | -1.6 | -3.0 | +2.2 | +7.0 | +9.0 | +3.7 | -0.4 | -2.1 | -2.8 | -2.7 | -1.3 | 0 |
| ts-s3000 | 10 | -1.6 | -3.0 | -0.2 | -1.5 | -1.0 | -1.8 | +1.2 | +0.5 | +0.6 | +0.0 | +1.4 | +3.1 | +4.3 | +8.0 | +5.6 | +3.4 | +0.1 | -1.8 | -0.4 | +0.0 | 0 |
| s10000 | 10 | +0.1 | +0.0 | +0.0 | -0.0 | -0.0 | -0.1 | +0.0 | -0.0 | -0.0 | -0.0 | -0.1 | -0.0 | -0.0 | +0.1 | +0.0 | +0.0 | +0.3 | -0.0 | +0.5 | +0.2 | 1 |
| b2 | 10 | +2.8 | -0.3 | +0.4 | +0.9 | -0.2 | -0.3 | -0.1 | +0.4 | +1.8 | +5.4 | +2.0 | +1.3 | +0.1 | +2.3 | -0.3 | +0.3 | +1.0 | -0.0 | +0.5 | +0.2 | 1 |
| b1 | 10 | +0.1 | +0.1 | +0.0 | -0.4 | +0.4 | -0.4 | -0.5 | +1.1 | +1.3 | -0.2 | -0.1 | -0.0 | +1.8 | +3.2 | +2.7 | +2.0 | +0.9 | +0.2 | +0.1 | -0.0 | 0 |
| ts-b1 | 10 | -0.3 | -0.3 | +0.5 | +0.6 | -0.2 | -0.4 | -0.3 | +0.1 | +0.1 | +0.2 | +0.6 | +1.1 | +3.0 | +3.6 | +4.7 | +3.3 | +1.0 | -0.2 | -0.1 | -0.6 | 0 |
| stats | 10 | -0.2 | -0.1 | +0.2 | -0.3 | -0.2 | -0.5 | -0.4 | +0.0 | -0.1 | +0.0 | -0.7 | +0.1 | -0.0 | -0.3 | +0.2 | +0.3 | +1.2 | -0.3 | -1.2 | -0.5 | 0 |
| stats-s300 | 10 | +0.6 | +0.5 | +0.6 | +0.6 | +0.8 | +0.9 | +0.6 | +0.3 | +0.3 | -0.2 | -0.1 | +0.5 | -0.2 | -0.7 | -0.4 | +0.1 | -0.1 | -0.3 | -0.9 | -0.5 | 3 |
| stats-s1000 | 10 | +2.6 | +0.6 | -1.3 | -1.9 | -0.1 | -0.1 | -0.2 | +1.1 | +0.2 | +0.3 | +1.4 | +0.3 | -0.3 | +1.1 | +0.6 | +0.4 | +2.4 | +1.4 | +0.8 | +3.5 | 0 |
| stats-s3000 | 10 | +0.0 | +0.3 | +0.3 | +0.3 | +0.3 | -0.0 | -0.2 | +0.5 | -0.1 | -0.0 | +0.4 | +0.2 | +0.0 | -0.8 | -0.5 | -0.7 | -1.0 | -1.1 | -1.0 | -0.9 | 1 |
| stats-b2 | 10 | -0.4 | -0.1 | -0.7 | +0.2 | -0.6 | -0.1 | -0.0 | -0.4 | -0.4 | -0.2 | +0.3 | -0.1 | +0.5 | +0.4 | +0.1 | +0.0 | +0.7 | +0.3 | -0.1 | -0.3 | 0 |
| stats-b1 | 10 | +0.4 | -0.2 | -0.4 | +0.1 | -0.0 | +0.1 | -0.4 | -0.4 | -0.4 | -0.2 | -0.2 | +0.2 | -0.2 | -0.1 | -0.0 | +0.5 | -0.0 | -0.2 | -0.3 | -0.1 | 1 |

**The time series** (`ts*` images: per batch of 500 calls, its start time, its ruler, its median). Batch median relative to the boot's median, mean over 10 boots, in 4 ms bins since the measurement began. The lead column is the time from the busy threads' start to the measurement:

Batch median relative to the boot's median (%), mean over boots, by time since the measurement began (ms):

| image | boots | lead (busy start to measurement), ms | 0 | 4 | 8 | 12 | 16 | 20 | 24 | 28 | 32 | 36 | 40 | 44 | 48 | 52 | 56 | 60 | 64 | 68 | 72 | 76 | 80 | 84 | 88 | 92 | 96 | 100 | 104 | 108 | 112 | 116 | 120 | 124 | 128 | 132 | 136 | 140 | 144 | 148 | 152 | 156 | 160 | 164 | 168 | 172 | 176 | 180 | 184 | 188 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ts | 10 | 5 | +1.8 | +1.9 | -0.5 | +0.3 | +1.1 | +0.4 | +1.5 | +1.5 | +1.6 | +0.7 | -1.4 | +3.4 | +2.7 | +4.1 | +3.4 | +3.9 | +2.2 | +1.2 | +0.9 | -0.7 | -1.2 | +1.1 | -0.6 | +0.2 | -0.1 | -0.3 | +0.8 | +0.1 | +0.9 | -0.0 | -0.4 | +2.0 | -0.1 | +0.5 | -0.4 | -0.7 | +1.8 | -0.1 | +0.5 | +1.4 | -0.4 | +0.9 | -0.8 | +0.1 | +0.4 | -0.2 | -0.9 |  |
| ts-lead300 | 10 | 304 | -0.7 | -0.0 | -0.6 | +0.2 | +0.4 | +0.6 | +1.7 | -0.1 | +1.0 | +0.8 | +0.0 | +1.4 | -0.3 | +0.3 | +1.2 | +0.1 | +1.1 | +0.1 | +0.2 | +0.8 | -0.5 | +0.9 | +0.9 | +0.9 | +0.8 | +0.2 | +1.4 | +0.2 | +0.3 | +0.6 | +0.1 | +1.1 | -0.5 | -0.1 | -0.1 | -0.8 | +0.1 | -1.1 | +0.2 | -0.2 | +0.8 | +1.9 | +1.3 | +2.2 | +3.2 | +0.7 | -5.2 | -4.8 |
| ts-lead1000 | 10 | 1002 | +0.3 | +0.6 | -0.7 | +0.4 | +0.4 | -0.8 | +1.2 | -0.0 | +0.4 | +1.1 | +0.1 | +0.6 | -0.3 | -1.0 | -1.4 | -1.0 | -0.4 | +0.3 | -0.2 | -0.2 | -0.0 | +0.1 | +0.7 | +1.8 | +1.5 | +1.2 | +3.6 | +2.4 | +1.6 | -0.6 | -1.0 | -1.1 | -1.4 | -1.2 | -0.9 | -1.7 | -1.5 | -1.9 | -1.4 | -0.6 | -0.9 | +0.3 | -0.1 | -0.8 | -2.0 | -1.7 | -0.4 |  |
| ts-depth | 10 | 5 | +1.6 | +1.7 | -1.5 | +1.0 | +2.8 | +1.0 | +3.3 | +1.7 | +2.3 | +2.0 | +1.0 | +6.4 | +5.9 | +4.7 | +1.3 | -0.4 | +1.6 | +0.7 | +0.8 | +0.1 | -1.5 | +1.3 | -1.1 | -0.0 | +0.3 | -0.0 | +0.4 | -0.4 | +0.3 | -0.3 | -1.5 | +1.1 | -0.7 | -0.5 | -1.2 | -1.8 | +0.1 | -1.2 | +0.3 | -0.8 | -1.7 | +0.6 | -1.3 | -0.2 | +0.9 | +0.2 | +0.8 |  |
| ts-b1 | 10 | 3 | -0.1 | -0.2 | +0.6 | +0.8 | +0.4 | -0.0 | -0.2 | +0.0 | -0.0 | +0.4 | +0.4 | +0.9 | +1.4 | +2.3 | +5.2 | +5.4 | +4.4 | +1.2 | +0.5 | +0.1 | +0.1 | -0.3 | -0.1 | -0.1 | +0.1 | -0.4 | -1.2 |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| ts-depth-b1 | 10 | 3 | -0.1 | +0.1 | +0.1 | +0.2 | +0.5 | -0.1 | +0.0 | +0.3 | +0.2 | -0.2 | +0.2 | +1.8 | +5.6 | +6.1 | +5.0 | +2.3 | +0.6 | -0.3 | -0.5 | -0.2 | -0.4 | -0.0 | +0.5 | +0.2 | +0.2 | +0.3 | -0.3 |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| ts-s3000 | 10 | 3 | -1.3 | -3.6 | -0.7 | -2.5 | -0.8 | -2.4 | -1.6 | +0.6 | -1.4 | +0.2 | -0.1 | -0.5 | +3.3 | +4.9 | +8.0 | +6.8 | +4.0 | +1.1 | -2.5 | +0.0 | -1.5 | -1.4 | +1.4 | -6.6 |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |

Ruler (TSC counts for 12 000 CPU cycles), mean over boots, same bins:

| image | 0 | 4 | 8 | 12 | 16 | 20 | 24 | 28 | 32 | 36 | 40 | 44 | 48 | 52 | 56 | 60 | 64 | 68 | 72 | 76 | 80 | 84 | 88 | 92 | 96 | 100 | 104 | 108 | 112 | 116 | 120 | 124 | 128 | 132 | 136 | 140 | 144 | 148 | 152 | 156 | 160 | 164 | 168 | 172 | 176 | 180 | 184 | 188 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ts | 7297 | 7265 | 7259 | 7272 | 7280 | 7270 | 7274 | 7284 | 7262 | 7264 | 7295 | 7324 | 7369 | 7469 | 7419 | 7450 | 7371 | 7369 | 7328 | 7349 | 7336 | 7357 | 7382 | 7399 | 7397 | 7395 | 7399 | 7401 | 7414 | 7417 | 7429 | 7429 | 7428 | 7431 | 7433 | 7431 | 7435 | 7434 | 7427 | 7429 | 7429 | 7426 | 7428 | 7432 | 7431 | 7427 | 7426 |  |
| ts-lead300 | 7304 | 7312 | 7312 | 7298 | 7305 | 7314 | 7313 | 7316 | 7345 | 7363 | 7352 | 7350 | 7352 | 7374 | 7360 | 7373 | 7378 | 7386 | 7403 | 7394 | 7401 | 7406 | 7423 | 7416 | 7430 | 7426 | 7428 | 7447 | 7437 | 7436 | 7468 | 7455 | 7448 | 7446 | 7449 | 7447 | 7449 | 7449 | 7450 | 7456 | 7460 | 7478 | 7489 | 7467 | 7475 | 7481 | 7433 | 7436 |
| ts-lead1000 | 7298 | 7300 | 7299 | 7307 | 7326 | 7345 | 7328 | 7337 | 7321 | 7332 | 7349 | 7326 | 7348 | 7346 | 7362 | 7349 | 7361 | 7365 | 7358 | 7363 | 7373 | 7376 | 7413 | 7426 | 7469 | 7441 | 7457 | 7424 | 7419 | 7441 | 7444 | 7456 | 7449 | 7454 | 7466 | 7466 | 7469 | 7466 | 7465 | 7463 | 7470 | 7465 | 7494 | 7497 | 7497 | 7476 | 7420 |  |
| ts-depth | 7266 | 7270 | 7261 | 7264 | 7264 | 7264 | 7263 | 7283 | 7294 | 7296 | 7299 | 7399 | 7381 | 7460 | 7379 | 7350 | 7314 | 7285 | 7300 | 7304 | 7341 | 7348 | 7348 | 7316 | 7311 | 7314 | 7350 | 7351 | 7382 | 7382 | 7382 | 7404 | 7401 | 7413 | 7418 | 7424 | 7430 | 7433 | 7436 | 7445 | 7450 | 7446 | 7448 | 7449 | 7446 | 7451 | 7449 |  |
| ts-b1 | 7021 | 7020 | 7040 | 7054 | 7037 | 7024 | 7034 | 7036 | 7059 | 7052 | 7042 | 7054 | 7072 | 7090 | 7136 | 7127 | 7136 | 7088 | 7064 | 7065 | 7058 | 7078 | 7099 | 7088 | 7091 | 7114 | 7096 |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| ts-depth-b1 | 7007 | 6996 | 6997 | 7000 | 7005 | 7011 | 7009 | 7013 | 7016 | 7016 | 7016 | 7052 | 7140 | 7120 | 7140 | 7064 | 7041 | 7041 | 7029 | 7053 | 7090 | 7138 | 7157 | 7089 | 7096 | 7091 | 7093 |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |
| ts-s3000 | 7273 | 7265 | 7268 | 7267 | 7268 | 7264 | 7267 | 7258 | 7266 | 7279 | 7268 | 7290 | 7348 | 7403 | 7432 | 7491 | 7411 | 7324 | 7284 | 7285 | 7307 | 7310 | 7304 | 7342 |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |  |

## Item 2: stats build (`stats`, N = 0), per boot

Lock figures are in CPU cycles (TSC counts × 12 000 / the contended ruler), over the contended measurement's window.
- *Lock utilisation* = all holds / wall time.
- *Held by busy CPUs* = the busy CPUs' holds / wall time: what the client can wait behind.
- *Waiting share* = time in `lock()` / (wall × CPUs).
- *Depth* = tickets ahead at asking.

| boot | p50 | worst round | round spread | client vCPU | lock utilisation (held / wall) | held by busy CPUs / wall | waiting share | free gap per handover | client hold mean / p99 | client wait mean / p99 | busy hold mean / p99 | busy wait mean / p99 | busy acq per client acq | client depth 0/1/2/3 (%) | busy depth 0/1/2/3 (%) | client wait at depth 1/2/3 | 2 x (client wait + hold) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 8 | 9024 | 9136 | 2.5 % | 1 | 69.3 % | 41.7 % | 75.8 % | 362 | 1321 / 1510 | 2977 / 3240 | 653 / 792 | 3772 / 4093 | 1.02 | 0.0/0.0/0.2/99.8 | 0.0/0.0/2.0/98.0 | -/2481/2978 | 8595 |
| 3 | 9104 | 9488 | 5.6 % | 0 | 69.7 % | 41.1 % | 75.8 % | 365 | 1391 / 1588 | 3084 / 3370 | 657 / 771 | 3829 / 4141 | 1.02 | 0.0/0.0/0.2/99.8 | 0.0/0.0/1.8/98.2 | -/2530/3085 | 8950 |
| 2 | 9120 | 9248 | 4.2 % | 3 | 70.5 % | 40.4 % | 75.6 % | 346 | 1427 / 1653 | 2938 / 3203 | 628 / 749 | 3736 / 4085 | 1.02 | 0.0/0.0/0.2/99.8 | 0.0/0.0/2.0/98.0 | 39918/2406/2936 | 8731 |
| 10 | 9136 | 9264 | 2.8 % | 0 | 69.6 % | 40.8 % | 75.9 % | 366 | 1399 / 1576 | 3075 / 3371 | 652 / 766 | 3837 / 4142 | 1.02 | 0.0/0.0/0.1/99.9 | 0.0/0.0/1.8/98.2 | 11743/2554/3076 | 8949 |
| 6 | 9168 | 9264 | 2.6 % | 2 | 70.9 % | 39.8 % | 75.6 % | 352 | 1527 / 1730 | 2987 / 3203 | 638 / 766 | 3875 / 4188 | 1.02 | 0.0/0.0/0.1/99.9 | 0.0/0.0/2.0/98.0 | 92316/3139/2985 | 9026 |
| 1 | 9312 | 9552 | 5.3 % | 3 | 71.1 % | 39.9 % | 75.6 % | 353 | 1537 / 1782 | 3009 / 3241 | 646 / 766 | 3907 / 4235 | 1.02 | 0.0/0.0/0.2/99.8 | 0.0/0.0/2.0/98.0 | 100214/2794/3006 | 9093 |
| 7 | 9312 | 9536 | 4.8 % | 3 | 71.2 % | 39.8 % | 75.6 % | 346 | 1531 / 1769 | 2951 / 3241 | 633 / 757 | 3851 / 4231 | 1.02 | 0.0/0.0/0.2/99.8 | 0.0/0.0/2.0/98.0 | 50026/2492/2949 | 8964 |
| 4 | 9440 | 9616 | 5.1 % | 3 | 71.6 % | 40.4 % | 75.6 % | 345 | 1536 / 1792 | 3003 / 3288 | 651 / 774 | 3888 / 4285 | 1.02 | 0.0/0.0/0.2/99.8 | 0.0/0.0/2.0/98.0 | 54784/2647/3001 | 9078 |
| 5 | 9504 | 9552 | 3.2 % | 2 | 71.5 % | 39.8 % | 75.5 % | 353 | 1598 / 1821 | 3049 / 3267 | 656 / 788 | 3977 / 4322 | 1.02 | 0.0/0.0/0.2/99.8 | 0.0/0.0/2.0/98.0 | 28584/2919/3048 | 9294 |
| 9 | 9504 | 9680 | 6.2 % | 2 | 71.7 % | 39.7 % | 75.5 % | 351 | 1610 / 1835 | 3023 / 3257 | 652 / 788 | 3976 / 4334 | 1.02 | 0.0/0.0/0.2/99.8 | 0.0/0.0/2.0/98.0 | -/2425/3023 | 9266 |

r with the contended p50 over 10 boots:

| figure | r | min-max |
|---|---|---|
| lock utilisation | +0.91 | 69.3-71.7 |
| held by busy CPUs / wall | -0.73 | 39.7-41.7 |
| waiting share | -0.76 | 75.5-75.9 |
| free gap | -0.55 | 345.3-366.1 |
| client hold mean | +0.92 | 1320.9-1609.9 |
| client hold p99 | +0.92 | 1510.5-1835.2 |
| client wait mean | +0.11 | 2938.2-3084.1 |
| client wait p99 | -0.10 | 3202.6-3370.9 |
| busy hold mean | +0.21 | 628.0-656.7 |
| busy hold p99 | +0.32 | 749.0-791.8 |
| busy wait mean | +0.87 | 3736.2-3977.1 |
| busy wait p99 | +0.97 | 4085.0-4333.9 |
| busy acq per client acq | +0.43 | 1.0-1.0 |
| 2 x (client wait + hold) | +0.88 | 8595.3-9294.0 |
| client depth 3 share | -0.07 | 99.81-99.86 |
| client vCPU | +0.50 | 0-3 |

**Reading.**
- The lock is 69-72 % held. The busy CPUs hold it 40 % of the time; the client's own holds make up the rest.
- The queue is always full: the client is behind 3 in 99.8 % and the busy CPUs in 98 %.
- The p50 follows the client's own hold (r = +0.92), which is longer on vCPUs 2 and 3 (1 530-1 610 cycles) than on 0 and 1 (1 320-1 400).
- It also follows the busy CPUs' wait (+0.87 mean, +0.97 p99). Their wait contains the client's holds.
- It does not follow the client's queue depth (r −0.07) or the waiting share (r −0.76, the opposite sign to saturation).
- The stats build spreads 5.2 % across boots.

The same window without the TSC (`depth`, N = 0): tickets ahead per boot, and the client's placement:

| boot | p50 | worst round | round spread | client vCPU | busy vCPUs | client 0/1/2/3+ (%) | busy 0/1/2/3+ (%) | busy acq per client acq | client mean ahead, rounds min-max | r(round p50, round mean ahead) |
|---|---|---|---|---|---|---|---|---|---|---|
| 6 | 7008 | 7488 | 11.4 % | 3 | 0,1,2 | 0.00/0.00/0.96/99.03 | 0.00/0.00/2.54/97.46 | 1.025 | 2.985-2.995 | +0.32 |
| 3 | 7024 | 7632 | 13.3 % | 3 | 0,1,2 | 0.01/0.00/1.01/98.98 | 0.00/0.00/2.56/97.44 | 1.025 | 2.980-2.995 | +0.39 |
| 9 | 7056 | 7600 | 11.6 % | 3 | 0,1,2 | 0.00/0.00/1.20/98.80 | 0.00/0.00/2.62/97.38 | 1.026 | 2.983-2.994 | +0.46 |
| 2 | 7104 | 7584 | 10.4 % | 0 | 1,2,3 | 0.00/0.00/0.19/99.81 | 0.00/0.00/2.36/97.64 | 1.023 | 2.997-2.999 | +0.40 |
| 4 | 7104 | 7440 | 7.9 % | 2 | 0,1,3 | 0.00/0.00/0.52/99.47 | 0.00/0.00/2.87/97.12 | 1.028 | 2.992-2.998 | -0.11 |
| 1 | 7136 | 7360 | 11.7 % | 3 | 0,1,2 | 0.01/0.00/1.30/98.69 | 0.00/0.00/2.61/97.38 | 1.026 | 2.978-2.994 | +0.79 |
| 7 | 7168 | 7344 | 5.1 % | 2 | 0,1,3 | 0.00/0.00/0.54/99.46 | 0.00/0.00/2.83/97.16 | 1.028 | 2.991-2.998 | +0.09 |
| 8 | 7168 | 7424 | 8.3 % | 0 | 1,2,3 | 0.00/0.00/0.14/99.86 | 0.00/0.00/2.25/97.74 | 1.022 | 2.998-2.999 | -0.21 |
| 5 | 7232 | 7440 | 5.5 % | 2 | 0,1,3 | 0.00/0.00/0.39/99.60 | 0.00/0.00/2.79/97.21 | 1.027 | 2.990-2.999 | +0.05 |
| 10 | 7248 | 7680 | 9.9 % | 1 | 0,2,3 | 0.00/0.00/0.45/99.55 | 0.00/0.00/2.46/97.53 | 1.024 | 2.991-2.999 | +0.53 |

By placement (client vCPU; busy vCPUs):

| client vCPU | busy vCPUs | boots | p50 each | median |
|---|---|---|---|---|
| 3 | 0,1,2 | 4 | 7008, 7024, 7056, 7136 | 7040 |
| 0 | 1,2,3 | 2 | 7104, 7168 | 7136 |
| 2 | 0,1,3 | 3 | 7104, 7168, 7232 | 7168 |
| 1 | 0,2,3 | 1 | 7248 | 7248 |

Across boots: r(p50, client mean tickets ahead) = +0.56; pooled over all rounds of all boots: r = +0.39.

## Method

- **Tree.** `~/joist-ct2` on the build VM: 8241b56, identical to `~/joist` (`diff -r` empty) before my edits. Pristine copies of the three files I edited are in `tools/orig/`. My changes are all behind flags (`tools/joist-ct2.patch`; the edited files are in `tools/work/`):
  - `ipcbench.c`:
    - `BENCH_ROUNDS`: each round's p50/p99 and its 128-cycle histogram (from the least time to the pooled p99.9, the rest counted in a last column), printed after the measurement;
    - `BENCH_SERIES`: per batch of 500 calls, its TSC offset, its ruler and its median (32-cycle buckets);
    - `BENCH_BUSY_SPIN=N`: the role-2 loop spins N TSC counts before each `cap_delete`;
    - `BENCH_LOCK_DEPTH`: system call 99 with op 0 at the start, op 3 at each round, op 1 at the end.
  - `benchroot.c`:
    - `BENCH_BUSY=k`: only k busy threads;
    - `BENCH_LEAD_MS`: a sleep (a timed-out Recv, as the settling sleep) between starting the busy threads and the contended measurement;
    - `BENCH_SERIES`: TSC notes at both points.
  - `run.c`:
    - under `RUN_LOCK_STATS` (already in the tree): tickets ahead at asking with the wait per depth, and the caller's CPU;
    - new `RUN_LOCK_DEPTH`: per round and CPU, how many acquisitions found 0/1/2/3+ tickets ahead, with no TSC (one extra load of `owner` after the `fetch_add`, two increments, before the spin).
  - The light kernels' `.text` is byte-identical to a build of the untouched `run.c` (`d12ae3da…`). Rebuilding `gate`/`stats` after the later `RUN_LOCK_DEPTH` edit left both `.text`s unchanged.
- **Images.** All are the bench-gate entry (`ROOT_TASK=benchroot MODULES=ipcbench`, default `BENCH_CALLS`, 20 × 5 000) with the flags above (`tools/build.sh`, `build2.sh`, `build3.sh`). `gate` has no flags at all. `base-r2`/`gate-r2` are byte copies of `base`/`gate`, booted in a later campaign.
- **Boots.** `tools/runiso-cur.sh` is boot.sh.current's remote half, run directly on the P53 (this host; ssh to itself is refused by host-key checking). It has the same QEMU line and the zeroed `/dev/shm/joist-test-ram`, takes `flock /srv/joist/test/lock`, puts QEMU on 0,1,6,7 and moves vCPU n to host CPU 2+n. It also records the resulting affinities at the head of each log: vCPUs on 2, 3, 4, 5, everything else on 0,1,6,7 in every boot.
- **Campaigns.** `tools/campaign.sh` is contend's campaign generalised to any number of images. Each round boots each image once, and the starting image rotates by round. Before every boot it runs `check.sh` (stop on difference) and the governor check, and waits until the build VM is < 2 % busy over 5 s. A boot is kept only if the VM stayed < 2 % busy and the log has `ipcbench: done`. `/proc/interrupts` was captured before and after each boot, and the host frequency every 0.2 s.
  - Campaign 1: 15 images, 150 boots.
  - Campaign 2: 16 images, 160 boots.
  - Campaign 3: 7 images, 70 boots.
- **Host settings.** `check.sh` passed before and after each campaign and once more at the end: governor `performance` on 2-5, `default_smp_affinity` c3, only the known per-CPU NVMe queue IRQs on 2-5/8-11, build VM QEMU pid 9937 on 0,1,6,7. The files are `raw/check-*.txt`.
- **Build VM.** Nothing else ran on it during campaigns. Builds were done between campaigns.
- **Units.** ipcbench figures are its own CPU cycles. Lock times are converted with the contended line's ruler. The time series assumes a 2.6 GHz TSC (only the time axis depends on it).
- **Analysis.** `tools/analyze.py`, `tools/roundprofile.py` and `tools/series.py`. Full output is in `raw/tables.md`, `raw/roundprofile.md` and `raw/series.md`.

## Images (SHA-256)

```
2fd7d3368fb9828687dd129e9328590aa80f962b38fbc3115b31057e80e43021  b1.iso
f0aac0b3181e4b063a65ee713f27efe290499ef7de7fde3cd3d09a2cf6ceaee0  b2.iso
56d56e38ea939af4e0fea5278a757d2c088d2abb07ae92bdc529a161288ffcc4  base-r2.iso
56d56e38ea939af4e0fea5278a757d2c088d2abb07ae92bdc529a161288ffcc4  base.iso
81505384544403048d24ef4d3f0c5cb45fc7831dc910d1eb6c985f23f3d8d2a6  depth-b1.iso
f7a87fc7d10869c4cd3187ef0b3a0dc859bb469e5fcb385b7672795fa22fa333  depth-b2.iso
7bf537b6c2d03341740b19274315573ce9ce826046b6037a9d598080b625cacd  depth-s100.iso
e4636d7f0583cb5608463bb2a1f2e4cf450af4e495a5a1496aa3b1e7db6d64a7  depth-s1000.iso
c00b892beb2fba7519f3cc1ba76cb33f44e9e671376fbdb3e338db80b55ca01e  depth-s2000.iso
ea2a035f8afbf854bcfb1ea56914ef625410938c5df665b96043d6a01965f647  depth-s300.iso
893ac556f7895d3f43484dc3a6f9680ca7dee97c7cddfd9dea25b1203b39f806  depth-s3000.iso
b0c524f4eb7af7884cb20fd3e41420078e3198c8b2b060fb6202e092c4ddfe94  depth-s600.iso
6ac2b8ec3423358a491c25b4402ee911ce0999fc753915ac0bb036cbf869506f  depth.iso
098b952218762d1bfe672274e4b198f0efc50b7521e7c362b2d0495104c0dcc0  gate-r2.iso
098b952218762d1bfe672274e4b198f0efc50b7521e7c362b2d0495104c0dcc0  gate.iso
59c833c3bbbc023646f23750d74316a7558294e8cbcb889425d5e54c81089f66  s100.iso
bd68a8fef63295d27f31b408c2341daaf6f9572a3e593a9b8b5aca0f82145015  s1000.iso
194c9bf2dcb07d40164b6b6ffc4c6412f29326abfe6680fe669df862bc45c290  s10000.iso
2ba9fc2299363ac0669118e85886838a3926e5102df32438bd665719574003d1  s2000.iso
24b03cb57773617b2f4422360031c5f563285770bb094f94fba5c08edb9ecc7d  s300.iso
5c7076b242f134578c505203f0fd29fdd7a135cc1e0cef6457cf8f777a243e80  s3000.iso
45b6e4fb7918bd7e5ca44cc3725b428813e5130588da4c1a0f5a278673a4ebcf  s600.iso
127d45255e2dac0b51f3792ae807d911c54d7f5c730bf4e6964ee48426730204  stats-b1.iso
ffab3c7de8972de36934c0537be9dd99e4942b43f682bcc83c3f5755cbf7b85c  stats-b2.iso
960bf04c858baf6801a1c0fd1f7c7421ff5e412e49073068791a4392cc468b7f  stats-s1000.iso
bef8b036c429d5b801d5bcaa6c66cfb7d780f756287ca73fd40e85b09ea8a078  stats-s10000.iso
bc0f890469bcce62e55ab0623c68ab75f9c383d068f63251d1e73344dd9a94ac  stats-s2000.iso
21a3813ee91279010b5f38374ecf439823339448e3e4056de73d6e52d80123c0  stats-s300.iso
3b8e7892304eb417a9fcc4671a171a8137563cb379e4d9f7bbf28724e5bdebe8  stats-s3000.iso
8a5ac7c05c9c8b46f2ea1601424c8d6f1c36000d646e9305228494d34fdaad8a  stats-s600.iso
597e2dbed0c6c900b075335e1dc1ea38ebd75d2b266e91131d90e7738f724acf  stats.iso
a95a2d7b306539267c460c67bba2761b7bd4d75c8f2907cb3d72f06a2326e018  ts-b1.iso
b1639d8f908a3b92dd225b1ba79557934f428d4eb93af18c1735a3fa1e6b7985  ts-depth-b1.iso
4b38e28b775d0384e814c374840543eabfadbf964b67fec9d67124b4f6a514ba  ts-depth.iso
3935189a7406eff36a45d0ad49cb276d701bed8740b7d7a5fa1c540bd689d9e4  ts-lead1000.iso
db28bea46293ad76f1f7941aabff34de5b6fcc4ca023490ce9d5ac89f7d68063  ts-lead300.iso
047a4e26a211912bd381482567e7e9c615aecbb0d8db7e53133c6f58d4511336  ts-s3000.iso
246ed49cfdd1da34a2dff36e38d1021bf2aeb722fbdda92e4456fcf42beb6305  ts.iso
```

## Files

- `raw/IMG-N.log`: the serial log, with the runner's header and pins. Each has `.freq`, `.irq-before` and `.irq-after` beside it.
- `raw/campaign.log`, `raw/check-*.txt`, `raw/images.sha256`, `raw/smoke/` (the first smoke boots), and the tables named above.
- `iso/`: all images.
- `tools/`: `joist-ct2.patch`, `orig/`, `work/`, `build*.sh`, `runiso-cur.sh`, `campaign.sh`, `check.sh` (contend's, unchanged), and the three analysis scripts.
