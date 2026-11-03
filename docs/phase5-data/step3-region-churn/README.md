# Phase 5 step 3: region churn against requests to mm (P53, 2026-11-03)

0008 accepted the region capability (0030) on the condition that it is
"measured against RPCs to mm". This is that measurement: a `malloc`-like
churn, map *n* pages, write a word in each, unmap them, timed as one with
the TSC, for *n* = 1, 4, 16 and 64.

- **Region:** the client's own `region_map` of *n* pages of an extent it
  already has (one REGION_NEW and one REGION_GROW before the timing), the
  writes, `region_unmap`. No request to mm, no fault.
- **Requests:** `MM_REQ_MAP_ANON` of *n* pages, the writes (a demand-zero
  write fault each, which mm serves), `MM_REQ_UNMAP`.

Each is run twice:

- **table emptied:** nothing else in the last-level page table, so every
  unmap empties it and the kernel frees it (`prune`, 0030's *Settled in the
  model step*), and the next map (or fault) builds and zeroes a new one;
- **table kept:** a page kept mapped at the table's first entry (as an
  arena's header would be), the churn in the pages above it.

60 samples a line in a boot, its median taken; 3 boots on each of 1 and 2
CPUs; the table gives the median of the 3 boots' medians, in cycles (TSC).

## How

`user/mmclient/mmclient.c` (`churn()`, the end of MMC_BENCH) and
`user/mmbench/mmbench.c`, the default build (512-page pool), at the commit
that adds this directory. Built on joist-dev2 (the P53 has no limine) and
booted on the P53's own CPU by `tools/p53/boot.sh` (KVM, the virtual CPUs
pinned to host CPUs 2-5, 256 MB, the same host pages each boot), from the P53
itself (`P53_HOST=aditya@localhost`), the build VM idle and no other QEMU
running: `bench.sh` here (`build`, then `boot`). Logs: `p53-kvm-smp*-*.log`.

## Results (median of 3 boots' medians, cycles)

| pages | region, table emptied | requests, table emptied | ratio | region, table kept | requests, table kept | ratio |
|---:|---:|---:|---:|---:|---:|---:|
| **1 CPU** | | | | | | |
| 1  | 7 552  | 14 766  | 1.96 | 1 590  | 8 812   | 5.54 |
| 4  | 9 520  | 31 956  | 3.36 | 3 640  | 25 980  | 7.14 |
| 16 | 17 786 | 100 260 | 5.64 | 11 844 | 94 040  | 7.94 |
| 64 | 49 764 | 374 822 | 7.53 | 43 886 | 368 386 | 8.39 |
| **2 CPUs** | | | | | | |
| 1  | 7 552  | 15 540  | 2.06 | 1 644  | 9 206   | 5.60 |
| 4  | 9 758  | 32 882  | 3.37 | 3 684  | 26 858  | 7.29 |
| 16 | 18 104 | 103 298 | 5.71 | 11 864 | 97 060  | 8.18 |
| 64 | 50 742 | 386 328 | 7.61 | 43 872 | 379 840 | 8.66 |

The spread of a line's three boots (largest less smallest, over the
median) was at most 2.7 % on 1 CPU (1 page by requests, table kept) and
4.3 % on 2 CPUs (4 pages by region calls, table emptied).

For reference, in the same boots (1 CPU): a request's round trip
(`MM_REQ_ID`) 1 202, a demand-zero write fault 9 012 (Phase 4's step7-p53:
1 326 and 10 304).

## What it says

- **The region calls are 2 to 8.7 times cheaper**, more so the more pages:
  by requests each page costs a fault (about 5 700 cycles a page past the
  first, 1 CPU), by region calls about 670 (with the table kept: the
  write to a page just mapped, and its share of the map and unmap).
- **Page-table churn is most of a small region churn.** Emptying the table
  costs about 5 950 cycles a cycle on 1 CPU, the same on both paths (1 page:
  7 552 − 1 590 by region calls, 14 766 − 8 812 by requests; on 2 CPUs
  5 908 and 6 334): building and zeroing a table page, and the prune that
  frees it, scanning its 512 entries. With the table kept, a 1-page region
  churn is 1 590 cycles; with it emptied, 7 552. An allocator that keeps a
  page mapped in each table it churns in pays the lower figure; whether
  glibc's `malloc` over a region (Phase 5 step 7) does is for that step.
- The two paths don't do the same work: a page from mm is zeroed at each
  fault; a region page keeps what it last held across an unmap and a map
  (mm zeroed it once, at REGION_GROW). That is what an allocator reusing its
  own memory expects, and part of the difference.
