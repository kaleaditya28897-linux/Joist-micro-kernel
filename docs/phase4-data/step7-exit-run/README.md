# Step 7: the exit run (2026-10-30)

`tools/exit-run.sh`, at f8d55b5, with the sizes the owner decided (part
E7): `cowstress` N = 1000 rounds on 1, 2 and 4 CPUs, and 250 on the
checking kernel (2 CPUs); `mm`'s refinement test with M = 10 000 seeds under
plain, asan and ubsan, and 1 000 under msan and tsan. `summary` is the
script's own summary; each part's full output is its `.log`.

**Where it ran:**
- **The boots,** on the P53's own CPU (i7-9850H) through KVM
  (`P53=1`, `tools/p53/boot.sh`: one host CPU per virtual CPU, CPUs 2 to 5,
  pinned).
- **The refinement test,** on the workstation (i7-8665U, Void Linux,
  Clang 21.1.8).

**Result: every part passed.**

## cowstress

Each round forks trees of children (`cowworker`), writes and verifies
pages in each, and kills some of them mid-fault or mid-fork; after each
round every counter must be level and no task but `mm` left (part E5).
The figures are each log's last line.

| Run | Seed | Rounds | Time | Forks | Deepest | Writes | Verified | Kills mid-fault | Mid-fork | Stranded | Slow | For quota |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| KVM, 1 CPU | 1 | 1000 | 26 s | 14 973 | 6 | 998 266 | 62 003 | 4 331 | 1 833 | 0 | 0 | 0 |
| KVM, 2 CPUs | 2 | 1000 | 24 s | 14 865 | 6 | 993 518 | 62 003 | 4 264 | 1 854 | 0 | 0 | 0 |
| KVM, 4 CPUs | 4 | 1000 | 28 s | 15 043 | 6 | 1 000 634 | 62 003 | 4 296 | 1 862 | 0 | 0 | 0 |
| Checking kernel, KVM, 2 CPUs | 2 | 250 | 582 s | 3 773 | 6 | 247 952 | 15 503 | 1 084 | 471 | 0 | 0 | 0 |

- **"Stranded"** is `mm` giving no answer within 10 s after a kill: a
  failure. **"Slow"** is an answer later than 300 ms but within 10 s
  (b019356). There were none of either.
- **"For quota"** counts forks cancelled because the parent's quota
  couldn't pay for the child's thread (f8d55b5, part E9). None, at quota
  32; the first attempt of this run, at quota 8, failed on exactly this.

## The refinement test

`model/test`'s refinement property test of `mm` against its abstract
model (`JOIST_MM_SEEDS`). The coverage line counts the operations over all
seeds.

| Build | Seeds | Time | Reads checked | Writes | COW copies | Clones | Shares | Unmaps | Protects | Exits | Faults refused |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| plain | 10 000 | 550 s | 453 690 | 1 070 289 | 168 495 | 137 301 | 106 890 | 373 261 | 30 551 | 93 991 | 429 633 |
| asan | 10 000 | 823 s | the same | | | | | | | | |
| ubsan | 10 000 | 823 s | the same | | | | | | | | |
| msan | 1 000 | 126 s | 45 685 | 107 788 | 16 908 | 13 739 | 10 775 | 37 499 | 2 945 | 9 392 | 42 695 |
| tsan | 1 000 | 219 s | the same as msan | | | | | | | | |

The same seeds give the same operations under every build, so the counts
match between builds with the same M.
