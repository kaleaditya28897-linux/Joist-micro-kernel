# model/: the Phase 1 host-side semantic model

The kernel's object model (tasks, threads, capabilities, endpoints, badges,
notifications, lifetime notifications) as a C17 library, tested on the host
under every sanitizer. Scope, invariants (I1–I10) and order of work:
`docs/phase1-plan.md`.

**State:** scaffold only. `src/` and `include/joist/` are empty; the test
harness and the build are in place and verified.

## Build and test

```sh
make            # build and run the tests (plain)
make check      # the same tests under plain, ASan, UBSan, MSan and TSan
make sanity     # prove each sanitizer is live: a seeded bug must be caught
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
| `include/joist/` | the model's public API (becomes `docs/abi.md`) |
| `src/` | the model; core code obeys the kernel rules below |
| `test/harness.[ch]` | minimal in-tree test harness (`JT_TEST`, `JT_CHECK`, `JT_REQUIRE`) |
| `test/test_*.c` | tests; picked up automatically |
| `test/sanitizer-sanity.sh` | the sanitizer liveness check |

## Rules for `src/`

The model's core is meant to move into the kernel (plan §8.1), so it follows
the kernel rules (§5.1, 0003), and the tests may not rely on it doing
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
