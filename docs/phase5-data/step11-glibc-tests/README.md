# glibc's own tests at the Phase 5 exit

glibc 2.43's own tests, built (`make check run-built-tests=no`) and linked
static by `tools/phase5/glibc-tests-link.sh` as at step 7
(`../step7-glibc-tests/`), each booted alone under `user/glibcroot` by
`tools/phase5/glibc-tests-run.sh` with step 7's specs (arguments,
environment, stdin), on joist-dev2 (KVM), 1 CPU. Settings as step 7's:
`mm`'s 64 MiB pool, of which the program may have 32 MiB; 3 MiB stack;
60 s. 3 060 of 3 258 linked, as at step 7.

Two runs make the record (`results-specs-1cpu.txt`, a line a test):

1. Every test, against glibc at 6816e03 (the main arena's break in a
   region), 3 at once, the machine also running the exit's CBMC.
2. `malloc/` and `stdlib/` again (`results-malloc-stdlib-nodyn.txt`, 256
   tests, 4 at once, the machine otherwise idle), after the fix below: their
   lines replace run 1's.

| | step 7 (`results-specs-1cpu.txt`) | exit |
|---|---:|---:|
| PASS | 1 937 | 1 938 |
| UNSUPPORTED | 41 | 41 |
| FAIL | 1 010 | 1 003 |
| TIMEOUT | 16 | 21 |
| NO-STATUS | 56 | 57 |

Accepted by the owner as Phase 5's record (2026-11-20).

Ten tests changed from step 7:

| test | step 7 | exit | why |
|---|---|---|---|
| htl/tst-basic7 | FAIL | PASS | |
| htl/tst-robust10 | FAIL | PASS | |
| malloc/tst-memalign-3 | FAIL | PASS | built without clang's malloc built-ins (step 7, 2026-11-12) |
| htl/tst-robust1, 4, 7 | FAIL | TIMEOUT | they wait for `EOWNERDEAD`: robust mutexes now exclude (1b44e4e), and Joist can't yet notice an owner that died (open) |
| htl/tst-robust9 | PASS | TIMEOUT | the same; it passed at step 7 only because the lock didn't exclude |
| math/test-float128-fma | FAIL | TIMEOUT | not rerun; failing either way (a timeout on a loaded machine likely) |
| string/test-strncmp-nonarray | PASS | TIMEOUT | load: alone it passes (`rerun-alone.txt`) |
| malloc/tst-trim1-threaded-main | TIMEOUT | NO-STATUS | 10 000 blocks of up to 16 KiB: past the program's 32 MiB either way |

`rerun-alone.txt`: the seven of run 1 that got worse, rerun alone with
`mm` reporting each fault it refuses (`-DMM_REPORT_REFUSED`).

**The fix: stdlib/tst-qsort2** (PASS at step 7, NO-STATUS in run 1, PASS
in run 2). It sorts nine arrays of up to 9.5 MB. Each is above `malloc`'s
`mmap` threshold at first, but glibc raises the threshold each time an
`mmap`'d block is freed, so later arrays went to the main arena. The
break stops at 8 MiB (the owner's choice), so `malloc` extended the main
arena with `mmap`'d pieces, which it never gives back; over the nine sorts
the program reached its tree's 32 MiB and `mm` refused its next fault. On
the Hurd's lazily mapped 128 MiB break the same memory had stayed in the
break and been trimmed. With the dynamic threshold off it passes, first
shown with `GLIBC_TUNABLES=glibc.malloc.mmap_threshold=131072`. The owner
chose to turn it off for Joist (2026-11-20): `tools/phase5/glibc-patches/0013`
lets a configuration start `malloc` with `no_dyn_threshold` set, and
`glibc/sysdeps/joist/Makefile` sets it. In run 2 nothing else in `malloc/`
or `stdlib/` changed but stdlib/tst-strtod-round, a TIMEOUT in run 1 and a
FAIL (as at step 7) alone.
