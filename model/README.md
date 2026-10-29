# model/: the Phase 1 host-side semantic model

The kernel's object model (tasks, threads, capabilities, endpoints, badges,
notifications, lifetime notifications) as a C17 library, tested on the host
under every sanitizer. Scope, invariants (I1–I10) and order of work:
`docs/phase1-plan.md`.

**State:** Phase 1 is closed (`docs/phase1-exit.md`). Work items 1–6 are implemented and tested: capability
spaces and quotas, endpoints and synchronous IPC with deferred replies,
notifications and lifetime events with queued no-senders, tasks and threads
with task destruction, and a whole-model property test and libFuzzer harness
over all of it. Item 6's parameter choices are an accepted amendment to
decision 0001 (no bare `Send`). Progress is tracked in
`docs/phase1-plan.md` under *Progress*.

## Build and test

```sh
make            # build and run the tests (plain)
make check      # the same tests under plain, ASan, UBSan, MSan, TSan and small
make sanity     # prove each sanitizer is live: a seeded bug must be caught
make mutate     # prove the property tests catch each bug in test/mutations.txt
                #   (MUTATE_ONLY=regex for a subset, MUTATE_JOBS=n at once; default: all CPUs)
make fuzz       # libFuzzer over the whole model (FUZZ_TIME=seconds, default 60)
make fuzz-sanity  # prove the fuzzer crashes on a set of injected bugs
make tla        # TLC on tla/IpcDelete.tla: IPC racing a preemptible destruction
make tla-mutate # prove the TLA+ properties catch each injected bug
make cbmc       # CBMC preservation proofs for the capability and task operations
make format     # clang-format in place (style: ../.clang-format)
```

`make check` passing is the bar for every change. `make sanity` exists
because a sanitizer that's silently missing would let `make check` pass
vacuously. It compiles one known-bad program per sanitizer, requires the
sanitizer to catch it, and runs an unsanitized control to show the catch is
the sanitizer's.

## Layout

| Path | What |
|---|---|
| `../kernel/include/joist/core.h` | the model's public API (documented in `docs/abi.md`) |
| `../kernel/core/` | the model's code, which is the kernel's object layer since Phase 3 step 2; it obeys the kernel rules below |
| `test/harness.[ch]` | minimal in-tree test harness (`JT_TEST`, `JT_CHECK`, `JT_REQUIRE`) |
| `test/test_*.c` | tests; picked up automatically |
| `test/sanitizer-sanity.sh` | the sanitizer liveness check |
| `test/test_props_*.c` | property tests: seeded random operations against a shadow model, per area |
| `test/exec.[ch]`, `test/test_props_all.c` | the whole-model sequence executor and its seeded property test |
| `fuzz/fuzz_model.c` | libFuzzer entry point: the same executor, choices from fuzz input |
| `fuzz/make_seeds.c` | writes the seed corpus: property-test sequences in the fuzz input encoding |
| `test/mutations.txt`, `test/mutate.sh` | injected bugs the tests must catch |
| `test/fuzz-sanity.sh` | injected bugs the fuzzer must crash on |
| `cbmc/` | CBMC harness and runner: per-operation preservation proofs (needs `cbmc`) |
| `tla/` | TLA+ model of IPC racing a preemptible task destruction (needs `tlc`, see `docs/phase1-plan.md`, *Tooling*) |

Property tests take `JOIST_PROP_SEEDS`, `JOIST_PROP_STEPS` and, to replay a
reported failure, `JOIST_PROP_SEED`.  `mm`'s refinement test (`test_mm_refine.c`)
takes `JOIST_MM_SEEDS` (default 48; the exit run sets more) and, to replay,
`JOIST_MM_SEED`.

## Rules for `../kernel/core/`

The model's core is the kernel's code (plan §8.1; moved in Phase 3 step 2),
so it follows the kernel rules (§5.1, 0003), compiles freestanding (no C
library: `internal.h` supplies `memset`'s declaration and a bare-text stand-in
for the checkers' `snprintf`), and the tests may not rely on it doing
otherwise:

- no `malloc`/`free`: objects come from explicitly donated pools;
- no recursion, no VLAs (`-Wvla` is an error), fixed-width integer types;
- every operation's work is bounded independently of user-controlled sizes;
- every return value is checked; every `switch` has a `default`.

The test code (`test/`) may use the host freely.

## Build notes

- The compiler defaults to `clang` through the `$(origin CC)` guard (decision
  0002's Makefile rule); `make CC=gcc` still works for the plain build.
- The test binary depends on a stamp of the source list (`build/sources.list`),
  so adding *or removing* a test rebuilds it. Without the stamp, a deleted
  test lived on in a stale binary.
