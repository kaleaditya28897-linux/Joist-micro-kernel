# Phase 5 step 8: libports' test server on Joist

`tools/phase5/libports-test/tst-libports.c` (built by
`tools/phase5/libports-test.sh` against `tools/phase5/hurd-libs.sh`'s
libraries), run by `tools/phase5/glibc-tests-run.sh` on joist-dev2 under
KVM, seeds 1-7 on 1, 2 and 4 CPUs, with the kernel's `pending[]` fix.

- `runs-2026-11-14.txt`: a run a block, its result line and then each
  part's line as the test prints it (21 runs, 21 PASS). The errors are the
  Hurd's numbers: 1073741869 is `EOPNOTSUPP`, 1073741828 `EINTR`.

The debugging runs before the fixes, and the check of the check (the
notifier with the port's own make-send count: the race part failed in all
14 runs on 2 and 4 CPUs), are in the hand-off's record on the P53,
`/srv/joist/handoff/p5-libports/findings.md`; `docs/phase5-plan.md`,
"Step 8: libports on Joist, built and tested", summarises them.
