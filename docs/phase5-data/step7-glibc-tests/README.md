# glibc's own tests on Joist (Phase 5 step 7, the design's item 11 d)

glibc 2.43's own test programs (`make check run-built-tests=no`), linked
static by `tools/phase5/glibc-tests-link.sh` and each booted alone under
`user/glibcroot` by `tools/phase5/glibc-tests-run.sh`, on joist-dev2 (the
P53's build VM, KVM), at commit f763cf2. Each run: mm's 64 MiB pool, of
which the program may have 32 MiB; 64 regions a task; a 3 MiB stack; 60 s;
stdout on glibcroot's console; `--direct` (no fork) for support's driver.

| file | what |
|---|---|
| `results-1cpu.txt` | every test that linked (3060 of 3258), 1 CPU: a line a test, `SUBDIR/TEST RESULT` |
| `results-threaded-2cpu.txt`, `-4cpu.txt` | the 340 tests that make threads, on 2 and 4 CPUs |
| `causes-1cpu.txt` | each 1-CPU result that isn't PASS or UNSUPPORTED: `TEST RESULT CAUSE \| a line of its log` |

Results: PASS (exit 0), UNSUPPORTED (77), FAIL with the exit code (127 is
glibc's `abort`, which ends the program with `_exit (127)` while there are
no signals), TIMEOUT (the root's 60 s), NO-STATUS (the program's threads
ended with no exit status: mm refused a fault).

On 1 CPU: 1851 PASS, 40 UNSUPPORTED, 1099 FAIL, 15 TIMEOUT, 55 NO-STATUS.

Causes (`a-` would be Joist's own: none is left; the eight found are fixed,
`docs/phase5-plan.md`):

| cause | tests | what |
|---|---|---|
| `b-fs` | 334 | no filesystem (EIEIO from a null root port) |
| `b-dl` | 91 | dynamic loading, iconv modules |
| `b-ipc` | 65 | pipes, sockets, ptys |
| `b-locale` | 63 | locale files |
| `b-signal` | 48 | signals (abort, pause) |
| `b-fork`, `b-exec` | 46, 9 | fork and exec (refused) |
| `b-state` | 23 | another thread's registers (htl's cancel) |
| `b-wx`, `b-clock`, `b-entropy` | 6, 4, 3 | W+X mappings, CPU-time clocks, entropy |
| `c-args` | 212 | the arguments, environment or expected output a test's Makefile gives it |
| `c-stdin` | 91 | stdin: the console answers only `io_write` |
| `c-mem` | 32 | mm's 32 MiB a program, or 64 regions |
| `c-static` | 16 | linked static where glibc links it dynamic |
| `c-module` | 15 | a module over 2 MiB, or too much bss |
| `c-time` | 4 | the 60 s |
| `d-clang` | 84 | compiler-rt's float128 without FP exceptions (libm) |
| `d-hurd` | 21 | what htl doesn't do (process-shared objects, priority protocols) |
| `e` | 2 | not understood: malloc/tst-memalign-3, misc/tst-tsearch |

The causes come from the hand-off's classifier
(`/srv/joist/handoff/p5-glibc-tests/scripts/classify.py` on the P53),
whose rules and by-name cases are each traced in its `findings.md`, the
whole record; the boot logs are `runs/RESULTS-f10-1cpu.tar.gz` and
`runs/RESULTS-f10-smp.tar.gz` there.

On 2 and 4 CPUs the threaded tests differ from 1 CPU only in timing: which
of two refusals a thread meets first (FAIL 1 or 127), rt/tst-cputimer2 (a
race with a missing /dev/null) and htl/tst-cancel32 (a TIMEOUT or an
abort, by which cancel type the cancel meets).

## The second run: arguments, environment, stdin (2026-11-09)

After the owner's answers to the hand-off (2026-11-08): each test gets
the arguments, environment and stdin its Makefile gives it
(`tools/phase5/glibc-tests-spec.py`, from `make -n check
run-built-tests=yes`: 3496 recipes, 103 with a stdin, 14 with an expected
output, compared), glibcroot's console answers `io_read`, mm has 256
regions a task, `pthread_create` returns EAGAIN where htl asserted, and
the runs lend each task 384 page-table pages (96 ran out once a program
could make scores of threads). Commit b78f107, joist-dev2, 1 CPU:

| file | what |
|---|---|
| `results-specs-1cpu.txt` | the 3060 tests: **1937 PASS, 41 UNSUPPORTED, 1010 FAIL, 16 TIMEOUT, 56 NO-STATUS** |
| `causes-specs-1cpu.txt` | the 1082 not passing, by cause, as above |

Against the first run: 96 tests pass that didn't (86 for a stdin, 8 for
mm's regions, among them misc/tst-tsearch, no longer unknown; 1 for its
arguments; htl/tst-cond7). Worse: 5 elf tests whose expected output is
a shared library's constructors' (`c-static`: linked static); 3 that
"passed" with no arguments and now get theirs, files and locales
(io/test-stat2, io/tst-statvfs, posix/tst-fnmatch: `b-fs`, `b-locale`);
malloc/tst-trim1 and tst-trim1-threaded-main, which with 256 regions
reserve more than the program's 32 MiB and die at the first touch past
it (`c-mem`). Most of the arguments a Makefile gives name files, so
`c-args` became `b-fs` more than a pass.

14 lines of `causes-specs-1cpu.txt` are corrected by hand, marked `*`:
the classifier's rules don't know the new messages (pthread_create's
EAGAIN, an output comparison, a refused fault past the ceiling), and
each was read in its log. One is left unknown: malloc/tst-memalign-3.

## With page tables on demand (2026-11-10)

The same 3060 tests at 977adb2, mm lending each task its page tables as
it needs them (the default first run of 16, then runs of 16 from 1024
pages) instead of the fixed 384 of the second run: every result the same,
test for test, as `results-specs-1cpu.txt`.

## The threaded tests on 2 and 4 CPUs (2026-11-10)

The 340 tests that make threads at 163db30, each with its spec, on 2 CPUs
(4 boots at once) and 4 (2 at once): `results-specs-thr2.txt`,
`results-specs-thr4.txt`, with `causes-specs-thr2.txt`, `-thr4.txt` (the
1-CPU causes where the result is the same; `*` where read by hand).
152 PASS, 2 UNSUPPORTED, 179 FAIL, 5 TIMEOUT, 2 NO-STATUS on each, as on
1 CPU; four tests differ from 1 CPU, among them htl/tst-robust11, which
showed that robust mutexes don't exclude on Joist (`getpid ()` is 0:
`docs/phase5-plan.md`). The hand-off's record, with its reruns and gdb
logs: `/srv/joist/handoff/p5-threaded` on the P53.
