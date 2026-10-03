# Phase 1 data

Raw logs behind the numbers in `docs/phase1-plan.md` *Progress*. Every number
quoted there for a work item from 5 onwards is in one of these files, and every
file says how it was made, so any number can be re-derived and checked. (Work
items 1–4 quote `make check` and `make mutate` output that any checkout
reproduces exactly: the property tests are deterministic from their seeds.)

## Environment

| | |
|---|---|
| Host | the workstation: Void Linux, kernel 6.18.54_1, 4 cores |
| Compiler | clang 21.1.8, with its sanitizer runtimes and libFuzzer |
| Model | Joist `model/` at the commit that adds these files. Each log was made in a scratch copy of `model/` (without `build/`), checked identical to the committed tree except `model/README.md`, which was edited afterwards |

## Work item 5 (2026-10-03): `item5/`

All four were run at the same time, each in its own copy, from `model/`:

| File | Command | Lines | SHA-256 |
|---|---|---:|---|
| `make-check.log` | `make check` | 392 | `ff84399a550ac18f3e55aa3eca238621273866f1523e8282690bec1f6a060e0e` |
| `make-mutate.log` | `make mutate` | 37 | `a40dce036d064d0ccc3d63f78e692409b234d59189bd22a33306fa1c86965f4d` |
| `make-fuzz-sanity.log` | `make fuzz-sanity` | 17 | `1f234837c1b1068cd93c9ec811769c3570a80c268f286fe19b018a9df78854ec` |
| `make-fuzz-900s.log` | `JOIST_FUZZ_STATS=1 make fuzz FUZZ_TIME=900`, empty `build/fuzz-corpus` | 643 | `9fff9575c19cee26ab92b9647e80ce8064edb555a990ca69701bd55f1b672494` |

How to read them:

- **`make-check.log`**: the test runner's output for each build in turn
  (plain, asan, ubsan, msan, tsan, small). The whole-model property test
  prints a `coverage` line per operation (count per result code, then the
  worst work seen and its bound), a `wake-ups` line, and a summary. The plain
  build's block is the first. The I9 table and the wake-up counts in
  `docs/phase1-plan.md` come from it. The small build prints a different
  cycle count (9, not 18) because its tables are smaller.
- **`make-mutate.log`**: one line per injected bug in
  `model/test/mutations.txt`: `caught` with the first failure it caused,
  or `STALE`/`SURVIVED`. The 36 lines are all `caught`.
- **`make-fuzz-sanity.log`**: per injected bug, the seeded result (pass/fail)
  and, indented, whether an empty-corpus fuzzer found it in 120 s.
- **`make-fuzz-900s.log`**: the start time, the compiler, libFuzzer's own
  progress lines (`NEW`, `REDUCE`, `pulse`), its final `stat::` lines, and the
  executor's coverage dump (`JOIST_FUZZ_STATS=1`) with the total steps run.

## Work item 6 (2026-10-03): `item6/`

Inputs for the amendment to decision 0001. Collected on VM 111 (x86_64
Hurd, GNU Mach 1.8+git20260805-up-amd64, hurd-dev 1:0.9.git20260919-1,
mig 1.8+git20231217-11, gcc 4:16.1.0-3, Python 3.14.7) and analysed on the
workstation. The tools are in `tools/phase1/`, byte-identical at this commit.

| File | Command | SHA-256 |
|---|---|---|
| `mig-sizes.csv` | `migsizes.py OUT $D/{auth,exec_startup,fs,interrupt,io,msg,process,socket,term,tioctl,fsys}.defs`, `D=/usr/include/x86_64-gnu/hurd` (the 11 interfaces in the traces) | `3043e889e9bcb48d9ea18c4e27bcbb1e747312a16985afb85907b7a556605b98` |
| `trace-sizes.csv` | `tracesizes.py ~/trace/boot.txt ~/trace/shell.txt ~/trace/build.txt` | `af63ba32430ecbdde3af28df2e69783603a0e4e8d0968a0d16eff1f1a14d7a14` |
| `register-coverage.csv` | `regcover.py mig-sizes.csv trace-sizes.csv ../../phase0-data/amd64/hurd-rpc-counts.csv` (on the workstation) | `2293625fae21b44cce90e074ace9d40d370eb5806afc513323a8e995adc34ba3` |
| `simpleroutines.csv` | `simpleroutines.sh` | `29add2f3d8c5de1d7b93abca7ec41551e5b729fc0872dfa5533d3ff850151dab` |
| `portcounts.csv` | `sudo sh portcounts.sh`, 2026-10-03T09:25Z, 566 processes (564 readable), the VM up 1 day 23 h with the Phase 0 workloads' leftovers | `7207a39c5578157b02bdf970bafb3b6909ef441970e5ede325c94df0c4d1c19f` |

The traces are Phase 0's stage 8 x86_64 traces, the same files: `boot.txt`
`8279f58d…6d4427`, `shell.txt` `d486e68a…d1811`, `build.txt`
`a294185e…848332` (full sums in `docs/phase0-data/README.md`, stage 8). The
call counts are `docs/phase0-data/amd64/hurd-rpc-counts.csv`, unchanged
(checked byte-identical to the VM's copy).

How to read them:

- **`mig-sizes.csv`**: one row per field of every routine's Request and Reply
  struct as MIG generates them, with its size on x86_64 (from `sizeof`, in a
  program appended to MIG's own server stub) and its class: `header` (Mach
  framing Joist doesn't send), `inline`, `retcode`, `var` (inlined up to the
  size shown), `port`, `portvar`.
- **`trace-sizes.csv`**: calls per power-of-two size class (the upper bound,
  in bytes) for `io_read`'s requested amount, `io_write`'s written amount and
  `dir_lookup`'s path and retry name. rpctrace truncates strings to 16
  characters, so `>=16` means "16 bytes or more, length unknown".
- **`register-coverage.csv`**: for each capacity in 64-bit words, the share
  of each workload's calls whose request and reply both fit. The sizing
  rules (all conservative) are in `regcover.py`'s header. `unknown_routines`
  counts traced routines missing from `mig-sizes.csv`: 0.
- **`simpleroutines.csv`**: every MIG `simpleroutine` declared in
  `/usr/include/x86_64-gnu/hurd/*.defs`.
- **`portcounts.csv`**: per process, the port names `portinfo` lists, split
  by kind. A name with both receive and send rights counts in both columns.

Verification of the model changes, run as for item 5 (four scratch copies of
`model/`, identical to the committed tree except `test/mutations.txt` line
20, re-targeted afterwards):

| File | Command | Lines | SHA-256 |
|---|---|---:|---|
| `make-check.log` | `make check` | 428 | `e7ae019ab1defaf343c8846cbc3f424a30b36f15a3c83bb9c27cdb84d23523f1` |
| `make-mutate.log` | `make mutate` | 45 | `65847874e210c186b6dc24044216a23e71dd02989bb42ec2dcf6e620fec00a4c` |
| `make-fuzz-sanity.log` | `make fuzz-sanity` | 17 | `c3e30bdfbeec2dbf93eada6e54de89c1234411522cbbb2158b93d84a5865e9e7` |
| `make-fuzz-600s.log` | `JOIST_FUZZ_STATS=1 make fuzz FUZZ_TIME=600`, empty corpus plus seeds | 614 | `2e1e77580af3b82191d19dab28d7e41abb2b920a592c3d8ade258036bb12b911` |

`make-mutate.log` has 42 `caught` and one `STALE` ("I4 never fires": its
target was the line that signalled the client's badge, which this item
replaced). Re-targeted at the new signal line and run alone, it is caught:
`caught I4 never fires: seed 1 step 377 after delete: object 8: word 0x0/0x20`.
