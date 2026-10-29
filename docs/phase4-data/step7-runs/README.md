# Step 7, part E6: mm maps runs, before and after (2026-10-29)

`user/mmbench` as E2 ran it (`../step7-exit/README.md`: what each sample
is), before and after `mm` maps runs of pages with one `map` call. Each
figure is the median and the fastest of its samples, in TSC cycles.

- **Before:** this commit's tree with `servers/mm/mmlib.c` from 2a5b893 (one
  `map` call a page). **After:** this commit. So both have the same bench.
- **A new row, SHARE:** 60 SHAREs of 64 pages none of which is there yet,
  into a running child that accepted each (`mm` makes each page, maps it
  into the caller, then into the child; both sides unmap between). E2 had
  no SHARE figure. For it `mmbench`'s root now starts the client's child,
  as `mmroot` does.
- **Where:** under KVM on the build VM, joist-dev2, which is itself a
  virtual machine (nested), as E2's figures were: not the P53's.
- **Load:** another hand-off's bootstrap ran throughout (a QEMU guest of 6
  vCPUs, about one CPU busy; load 1.6 to 2.6 on 8 CPUs, 5.8 GB free;
  `load.txt`, taken before each boot). The VM was never idle while this was
  measured. Boots alternated, before then after, so both saw the same load.
- **Recipe:** `bench.sh` (the default build on 1 and 2 CPUs, 3 boots each;
  the large build, E2's, on 1 CPU, 3 boots each). Logs `before-*.log` and
  `after-*.log`.

## What changed in mm

A run is consecutive pages of the task's space, backed by consecutive
offsets of one frame, with the same permissions, within one table's span
(`MM_SPAN`, what one `map` call may cover). `mm` maps each run with one
call: a read fault's pages (up to 16), a clone's pages into the child, and
SHARE's new pages into the caller and all of them into the other task.
For pages to come out of the free list in order, `mm_unmap` and
`mm_task_gone` now give pages back last first.

## Results

The default build (median / fastest; run 1, 2, 3):

| 1 CPU | before | after |
|---|---|---|
| demand-zero read fault (16 pages) | 56 554 / 55 806, 57 874 / 57 260, 57 866 / 57 182 | 51 892 / 51 176, 62 348 / 51 350, 51 916 / 51 208 |
| SHARE, 64 pages | 269 758 / 263 058, 269 982 / 269 646, 269 990 / 269 654 | 215 108 / 213 512, 255 438 / 213 858, 221 556 / 213 898 |
| CLONE of 1 MiB | 175 580 / 174 072, 177 480 / 176 932, 180 018 / 177 516 | 88 878 / 65 568, 65 974 / 65 338, 109 550 / 96 332 |
| demand-zero write fault (one page) | 6 532 / 6 436, 6 674 / 6 610, 6 680 / 6 596 | 9 916 / 8 292, 6 698 / 6 630, 6 732 / 6 652 |

| 2 CPUs | before | after |
|---|---|---|
| demand-zero read fault (16 pages) | 58 876 / 57 628, 58 354 / 56 240, 58 738 / 56 476 | 50 634 / 50 040, 52 236 / 51 484, 49 504 / 48 856 |
| SHARE, 64 pages | 409 466 / 370 758, 378 410 / 341 836, 391 650 / 366 040 | 214 636 / 213 306, 273 958 / 271 624, 210 776 / 209 488 |
| CLONE of 1 MiB | 184 910 / 184 076, 182 912 / 178 944, 188 420 / 187 718 | 63 898 / 63 706, 65 968 / 65 486, 62 432 / 62 332 |

The large build (1 CPU, 1 GiB):

| | before | after |
|---|---|---|
| CLONE of 1 MiB | 998 972 / 872 450, 873 944 / 873 062, 890 452 / 852 084 | 854 888 / 747 844, 757 388 / 750 360, 859 826 / 747 770 |
| CLONE of 16 MiB | 3 240 196 / 3 143 638, 3 184 320 / 3 135 264, 3 287 228 / 3 214 362 | 1 421 678 / 1 319 550, 1 367 078 / 1 213 924, 1 412 324 / 1 368 254 |
| CLONE of 256 MiB | 40 410 320 / 39 750 876, 39 973 020 / 39 635 762, 39 879 340 / 39 629 348 | 10 019 696 / 9 951 954, 10 119 986 / 9 844 752, 10 196 212 / 9 928 616 |

The other rows (upgrade, copy, a request, protect, `nosenders_next`) match
before and after within the boots' spread, as they should: none of them
maps more than one page. See the logs.

## What they say

- **CLONE: about 4 times cheaper a page.** At 256 MiB, 40 M cycles before
  and 10.1 M after: about 600 cycles a page before (E2: 590), about 140
  after (less the large build's fixed cost of 0.75 to 0.87 M, E2's
  cleared task record). At 1 MiB on the default build, 177 000 to 66 000.
  What is left a page is the child's records and reference counts, and the
  kernel writing the entries.
- **The read fault: about a tenth cheaper.** 57 000 to 51 000 for 16
  pages: 15 `map` calls saved, about 400 cycles each. Zeroing the 16 pages
  is most of what is left (E2: 3 200 a page, and a demand-zero write of one
  page costs 6 600).
- **SHARE of 64 pages: a fifth cheaper on 1 CPU** (270 000 to 215 000),
  two calls instead of 128. It still protects pages already there one
  `protect` call each (none here: every page is new).
- **On 2 CPUs SHARE before cost 380 000 to 410 000**, against 270 000 on
  one; after, 211 000 to 274 000. Each `map` into the other task (whose
  thread runs on the other CPU) may cost the kernel more there; with one
  call instead of 64 that cost is paid once. A guess from these figures,
  not measured apart.
- **Noise:** a few boots have one row far above the others (after, 1 CPU,
  run 1: the write fault and CLONE; run 3: CLONE; 2 CPUs run 2: SHARE).
  The fastest samples stay with the other runs': that is load, not the
  code. Throw out boots like that before comparing; better, measure again
  on an idle machine, and on the P53.
