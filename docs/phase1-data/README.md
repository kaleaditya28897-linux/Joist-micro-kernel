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
