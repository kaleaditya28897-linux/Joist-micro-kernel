# Step 7: the exit measurements (2026-10-29)

`user/mmbench` (the root; mm is module 0, `mmclient` module 1, run as
`MMC_BENCH`): the Testing table's row "Measured, recorded". `mmclient`
times each operation with the TSC (`lfence; rdtsc` both sides) in its own
thread; `mmbench` times `nosenders_next` itself, on `mm`'s endpoint (it
holds it with RECV too). Each figure is the median and the fastest of its
samples, in TSC cycles.

**All of it under KVM on the build VM, joist-dev2, which is itself a
virtual machine** (QEMU/KVM nested inside it; an i7-9850H, TSC about
2.59 GHz). Not figures for the P53 itself: step 3 found the build VM's
fault round trip with a handler of its own context about six times the
P53's. A passive `mm` sets no timer on the fault path, which is most of
what nesting costs, so these may be nearer the P53's than that; measure on
the P53 before quoting them as its own.

## What each sample is

| Row | Sample | Samples |
|---|---|---|
| demand-zero write | a first write to a page of a fresh region: fault, `mm` takes a page, zeroes it, maps it (one `map`), replies | 240 |
| demand-zero read | a first read: as above, for the 16 pages around it (16 pages zeroed, 16 `map` calls, one fault) | 240 (one each 16 pages, over 8 fresh spans) |
| write, copy | a write to a page shared COW with a child (which has no thread): fault, `mm` copies it into a fresh page, `map_replace`, replies | 240 |
| write, upgrade | a write to a read-only page that is its last reference (the child destroyed): fault, `protect` of one page, reply | 240 |
| a request | `MM_REQ_ID`'s round trip: the least `mm` does (collection, then the answer) | 240 |
| protect, 512 pages | `MM_REQ_PROTECT` over one table's span, every page mapped: one `protect` call and its shootdown, round trip included | 60 each way |
| CLONE | `MM_REQ_CLONE`, round trip, every page of the region mapped (by reads), the child (no thread) destroyed between | 9, 5, 3 |
| `nosenders_next` | the call finding nothing queued, as `mm` makes it at the start of every message | 240 |

## Results

The default build (the user programs' `mm` sizes: 16 tasks, 512 pages a
task, a pool of 512 pages): 1 MiB clones at most (`MM_TASK_PAGES`). Three
boots each; logs `kvm-smp1-*.log`, `kvm-smp2-*.log`.

| | 1 CPU, run 1 | run 2 | run 3 | 2 CPUs, run 1 | run 2 | run 3 |
|---|---:|---:|---:|---:|---:|---:|
| demand-zero write fault | 6 110 / 6 018 | 5 958 / 5 880 | 5 960 / 5 892 | 6 244 / 6 166 | 6 690 / 6 620 | 6 252 / 6 180 |
| demand-zero read fault (16 pages) | 53 032 / 52 458 | 51 908 / 51 340 | 51 944 / 51 270 | 53 282 / 52 608 | 57 160 / 56 406 | 53 268 / 52 578 |
| write fault, copy (COW) | 8 616 / 8 536 | 8 410 / 8 312 | 8 430 / 8 334 | 8 732 / 8 638 | 9 372 / 9 294 | 8 750 / 8 668 |
| write fault, upgrade (last reference) | 2 900 / 2 870 | 2 828 / 2 790 | 2 824 / 2 792 | 3 040 / 3 008 | 3 258 / 3 214 | 3 042 / 3 008 |
| a request (`MM_REQ_ID`) | 1 190 / 1 182 | 1 164 / 1 158 | 1 170 / 1 166 | 1 244 / 1 238 | 1 338 / 1 330 | 1 266 / 1 254 |
| protect, 512 pages, read-only | 19 656 / 19 484 | 19 302 / 19 080 | 19 220 / 19 088 | 19 736 / 19 574 | 21 192 / 21 020 | 19 766 / 19 614 |
| protect, 512 pages, writable | 21 142 / 20 980 | 20 830 / 20 620 | 20 736 / 20 578 | 21 254 / 21 130 | 22 794 / 22 636 | 21 280 / 21 146 |
| CLONE of 1 MiB | 161 872 / 161 296 | 162 026 / 161 296 | 157 804 / 157 430 | 168 162 / 166 446 | 179 772 / 179 490 | 167 784 / 166 686 |
| `nosenders_next`, nothing queued | 250 / 248 | 246 / 242 | 246 / 244 | 272 / 270 | 290 / 286 | 272 / 268 |

(median / fastest.)

The large build, for the clones of 16 and 256 MiB (1 CPU, 1 GiB of guest
memory, logs `kvm-smp1-big-*.log`):

| | run 1 | run 2 | run 3 |
|---|---:|---:|---:|
| CLONE of 1 MiB | 934 466 / 878 348 | 864 770 / 818 072 | 862 410 / 815 606 |
| CLONE of 16 MiB | 3 153 056 / 3 128 394 | 3 190 434 / 3 087 952 | 3 087 370 / 3 034 304 |
| CLONE of 256 MiB | 38 470 982 / 37 930 516 | 38 769 504 / 38 374 068 | 37 998 634 / 37 949 458 |

Its other rows match the default build's within 2 % (see the logs). The
256 MiB clone is about 15 ms at the VM's 2.59 GHz.

The large build (`~/vlog4/bench.sh` on the VM ran both): `mm` with 4 tasks of 66 048 pages each,
a pool of 66 048 pages, `MM_TABLE_PAGES` 288 (a child's tables for 256 MiB:
2 + 2 per 2 MiB span); the root's memory two frames of 69 632 pages
(`RUN_POOL_FRAMES`, `RUN_POOL_PAGES`, which are now `#ifndef`s in
`kernel/arch/x86_64/run.h`), a larger object table and root budget, and
QEMU with 1 GiB. `mm` and the client are lent 288 more pages each for
their own tables (`MMB_TABLES`). Built as:

    make BUILD=build/bench-mm-big ROOT_TASK=mmbench MODULES="mm mmclient" \
      USER_BUILD=build/mmbench-big \
      UEXTRA="-DMMB_POOL=66048u -DMMB_CLONE_PAGES=65536u -DMMB_TABLES=288u" \
      MMFLAGS="-DMM_MAX_TASKS=4u -DMM_TASK_PAGES=66048u -DMM_PAGES=66048u -DMM_TABLE_PAGES=288u" \
      KEXTRA="-DRUN_POOL_FRAMES=2u -DRUN_POOL_PAGES=69632u -DROOT_BUDGET=16384u" \
      KSIZES="-DJOIST_MAX_OBJECTS=8192u -DJOIST_MAX_CSPACES=64u -DJOIST_MAX_THREADS=128u -DJOIST_MAX_LEAVES=256u" \
      build/bench-mm-big/joist.iso

What limits the default build: a task's records (`MM_TASK_PAGES` 512, 2
MiB), the root's memory (8 MiB in four frames of 2 MiB, and `mm`'s pool is
one frame), and a child's lent table pages (16: 7 spans). None is a limit
of the design; each is a compile-time size.

## What they say

- **Collection is cheap but not free.** `nosenders_next` finding nothing
  costs about 250 cycles (270-290 on two CPUs): about 21 % of the cheapest
  request's round trip (`MM_REQ_ID`, about 1 170), 9 % of an upgrade fault,
  3 % of a copy, 4 % of a demand-zero write. 0023's amendment promised this
  figure; if it matters, the alternative it names (collect only when the
  pool runs short, or every Nth message) would save it on every message
  but the ones that collect.
- **Faults.** An upgrade (one `protect`) is about 2 800 cycles, round trip
  included: close to step 3's passive figure on the P53 (2 600). A copy
  adds a fresh page, a 4 KiB `memcpy` and `map_replace`: about 8 500. A
  demand-zero write (`memset` and a `map`): about 6 000.
- **The read fault's 16 pages cost 16 `map` calls.** 52 000 cycles for 16
  pages, about 3 200 a page, against 6 000 for one page on a write fault:
  the round trip is paid once, but each page is zeroed and mapped one
  `map` call at a time. A run of pool pages that are contiguous in a
  frame could be mapped with one `map` (the kernel takes a run since
  0020); `mm`'s free list doesn't keep them contiguous today.
- **CLONE is linear, about 590 cycles a page** at 256 MiB (38.4 M / 65 536):
  a `map` call per page into the child (the downgrade is one `protect` per
  span). Runs of pages contiguous in a frame could be mapped in one call,
  as above.
- **A clone's fixed cost grows with `MM_TASK_PAGES`**: the large build's
  1 MiB clone takes 5 times the default build's (860 000 against 162 000)
  because `mm_task_add` and `mm_task_gone` clear a whole task record (about
  1 MiB at 66 048 pages). A task's map array sized for the largest task
  costs every task: a design point for the owner (per-task records from the
  pool, or the clear limited to `nmap`).
- **protect over 512 pages**: about 20 000 cycles, round trip and shootdown
  included (on one CPU there is no other CPU to shoot down; on two the
  child has no thread, so none either).
- Two CPUs cost 5-10 % more everywhere, run 2 more than the others: noise
  of the nested VM, not a trend these runs can show.
