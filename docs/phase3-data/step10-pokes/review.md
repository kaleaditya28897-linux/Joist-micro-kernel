# Review of decision 0018 (uncommitted, against 8241b56)

Reviewer: an independent Claude Code session, 2026-10-19. Review only: nothing in
`tree/` was changed. Experiments ran in copies under `review0018/scratch`,
`review0018/mut/*` and `review0018/gcchead` (my scripts: `mut.sh`, `newtest.c`,
`newtest2.c`). The build VM was not used.

**Summary.** I found no missed wake-up, livelock or out-of-bounds index in
`joist_sched_pokes` or in how run.c uses it (items 1–3). The findings are
about the tests and the build:

- One of the new mutations can't be compiled, so `make mutate` will fail.
- Two ordering rules from the decision are not tested: idle CPUs before busy
  ones, and stopped threads before running ones. A mutation that breaks
  either rule survives.
- The scan fallback is never run by any test.
- The new test breaks `make CC=gcc`.
- There is one small mismatch between the model and run.c, which does no
  harm.

Baseline: with clang, the whole model suite passes with the diff applied (174
tests, 0 failed). Six of the seven listed pokes/preempt mutations are caught;
the seventh doesn't compile (F1).

## Findings

### F1 — `pokes-unclaimed-ignored` doesn't compile, so `make mutate` fails (Medium, test setup)
`model/test/mutations.txt:189`. Replacing `if (!claimed(cl, n, x - 1u)) {` with
`if (1) {` leaves the `cl` and `n` parameters of `rq_first_unclaimed` unused.
With `-Werror` this gives `sched.c:635:94: error: unused parameter 'cl'` (and the
same for `n`). mutate.sh then reports `STALE ... mutant does not build`. That
result isn't `caught`, so the run exits 1. The skipping of claimed threads in
the queue walk is therefore not checked by mutation.

Fix (tested: the mutant builds and is caught at test_smp.c:142):
```
pokes-unclaimed-ignored @@ ../kernel/core/sched.c @@                 if (!claimed(cl, n, x - 1u)) { @@                 if (!claimed(cl, n, x - 1u) || 1) {
```

### F2 — "Idle CPUs before busy ones" is not tested (Medium, test weakness)
`kernel/core/sched.c:753` (the final loop over `order`). This mutation
survives every SMP test (`run-tests smp`; `joist_sched_pokes` is called only
from test_smp.c):
```
pokes-idle-after-busy @@ ../kernel/core/sched.c @@         uint32_t o = order[i]; @@         uint32_t o = order[(i + idle) % m];
```
With it, busy CPUs are asked before idle ones. Example state, 4 CPUs:

- CPU 0 (me) runs a (prio 5).
- CPU 1 is idle.
- CPU 2 runs l (prio 1).
- w (prio 3) becomes ready.

The correct mask is {1}. The mutant gives {2}: l is preempted, the preemption
re-sets the flag, and on the next pass l moves to CPU 1. The end state is
still settled, so the property tests pass. They check only that the CPUs
settle, and their interrupt limit (`sent > changed + flight0 + ncpus`,
test_smp.c:343) is loose enough to allow this. The unit test never has a
thread waiting that beats a busy CPU's thread while a CPU is idle.

The decision's second consequence ("an idle CPU is preferred to preempting a
busy one") is therefore not checked at all. The new test below catches this
mutant (test_smp.c:575 in the copy) and passes on the unmutated tree.

### F3 — "A thread that can't run goes last" in `goes_later` is not tested (Low–Medium, test weakness)
`kernel/core/sched.c:703`. This mutation survives the SMP tests:
```
pokes-stopped-asked-last @@ ../kernel/core/sched.c @@         return sa == NULL && sb != NULL; @@         return sa != NULL && sb == NULL;
```
Example state:

- CPU 0 (me) runs a (5).
- CPU 1 runs s, which can no longer run.
- CPU 2 runs l (1).
- CPU 3 runs b (5).
- w (3) is waiting.

The correct mask is {1}: CPU 1 takes w and l stays. The mutant asks CPU 2
first, so the mask is {1,2}: l is preempted for w, and CPU 1 is sent away to
go idle. The CPUs settle again later, so only the unit test could see this.
The unit test's case for a thread that can't run (test_smp.c:161-163) has
nothing waiting that beats a running thread, so the order doesn't matter there.

Proposed test, which catches both F2 and F3 and passes on the tree
(`review0018/newtest.c`):
```c
JT_TEST(smp_review_idle_before_busy_and_stopped_before_running)
{
    boot();
    joist_thread a = thread_with(10, 5, 10, 10);
    JT_CHECK(pick(0) == a);
    joist_thread s = thread_with(11, 5, 10, 10);
    JT_CHECK(pick(1) == s);
    joist_thread l = thread_with(12, 1, 10, 10);
    JT_CHECK(pick(2) == l);
    joist_thread b = thread_with(13, 5, 10, 10);
    JT_CHECK(pick(3) == b);
    joist_thread w = thread_with(14, 3, 10, 10);
    JT_CHECK(joist_sched_pokes(&k, 0, 0, 4) == 1u << 2); /* all busy: the worst, l */
    k.resched = 0;
    JT_REQUIRE(joist_sc_unbind(&k, root, 11) == JOIST_OK);
    JT_CHECK(joist_sched_pokes(&k, 0, 0, 4) == 1u << 1 && pick(1) == w); /* l not preempted */
    k.resched = 0;
    JT_REQUIRE(joist_sc_unbind(&k, root, 13) == JOIST_OK);
    JT_CHECK(pick(3) == JOIST_MAX_THREADS);
    (void)thread_with(15, 3, 10, 10);
    JT_CHECK(joist_sched_pokes(&k, 0, 0, 4) == 1u << 3); /* the idle CPU, not l's */
}
```

### F4 — The scan fallback in `choice()` is never run by a test (Low, test weakness / unused code)
`kernel/core/sched.c:680-690`. The first loop at sched.c:667 is also skipped
when `rq_npend != 0`. The tests call `joist_sched_pokes` only after `pick`,
which flushes the queues, so `rq_npend` is always 0 there. The kernel likewise
calls it only after settling. Both of these mutations survive:
```
pokes-scan-ignores-claims @@ ../kernel/core/sched.c @@         if (sc == NULL || k->th[i].cpu != 0 || claimed(cl, n, i)) { @@         if (sc == NULL || k->th[i].cpu != 0 || (claimed(cl, n, i) && 0)) {
pokes-scan-dropped @@ ../kernel/core/sched.c @@     for (uint32_t i = 0; i < JOIST_MAX_THREADS && k->rq_npend != 0; i++) { @@     for (uint32_t i = 0; i < JOIST_MAX_THREADS && k->rq_npend != 0 && 0; i++) {
```
The kernel never runs this code, so neither mutant is a kernel bug today. But
the fallback is code that would be wrong without anyone noticing if a future
caller asked while the queues are held. There are two fixes:

- Test it. The following catches both mutants and passes on the tree
  (`newtest2.c`):
  ```c
  boot(); joist_thread a = thread_with(10, 5, 10, 10); JT_CHECK(pick(0) == a);
  joist_sched_hold(&k);
  joist_thread x = thread_with(11, 5, 10, 10);
  JT_REQUIRE(k.rq_npend != 0);
  JT_CHECK(joist_sched_pokes(&k, 0, 0, 4) == 1u << 1);
  JT_CHECK(joist_sched_pokes(&k, 0, 1u << 2, 4) == 0);
  joist_sched_settle(&k); JT_CHECK(pick(1) == x);
  ```
- Or remove the fallback and require `rq_npend == 0` (as a checked
  precondition, returning a full mask, for example).

### F5 — The new test breaks `make CC=gcc` (Low, build)
`model/test/test_smp.c:331`: `rng = rng0 + (uint64_t)order * 0x9E3779B97F4A7C15ULL;`.
GCC 14.2.1 rejects it: `error: conversion to 'long long unsigned int' from
'long unsigned int' may change the sign of the result [-Werror=sign-conversion]`.
HEAD (the diff reverse-applied) builds with gcc without errors.
`model/README.md:77` says `make CC=gcc` still works for the plain build. Clang,
the default compiler, accepts the line.

This version builds with gcc and the SMP tests pass:
`rng = rng0 + (uint64_t)(unsigned)order * (uint64_t)0x9E3779B97F4A7C15ULL;`

### F6 — Where run.c differs from what the model drives (Low; no wrong outcome found)
`kernel/arch/x86_64/run.c:1436` (`c->poked = 0` at every kernel entry),
`:1170` (idle) and `:1484-1508` (the fast path and the `stays` exit, which
skip `poke_others`).

In run.c, `poked` is cleared at *any* entry under the lock, not when the
reschedule interrupt is taken. A CPU that was poked but entered first through
a system call (it was already waiting for the lock, which is common in the
convoy) clears `poked` while the interrupt is still pending in its LAPIC. It
can then leave through the fast path or `stays` without choosing. There are
two consequences:

- From then on, `inflight` does not include that CPU. If another CPU decides
  in that window, the thread may be asked of a CPU again, and that interrupt
  merges with the pending one. So the property test's `resent == 0`
  (test_smp.c, "nobody was interrupted twice") is a property of the model,
  not of the kernel.
- In the fast-path case, the claimed thread is picked up only because the
  hardware interrupt is still pending (it arrives on return to user mode).
  The model has no "interrupt still pending after the flag is cleared" state,
  and `kernel_choose` always picks. So the tests drive a copy of
  `poke_others` (`kernel_exit`), not run.c's entry and exit paths.

I could not turn either of these into a missed wake-up or a wrong placement:

- The handed-over thread inherits the caller's context and place, so it ranks
  exactly as the caller did and no other CPU's choice changes.
- `stays` requires the scheduler to choose the same thread.
- An idle CPU's `poked` is always cleared just before it picks.
- `poked == 1` implies the CPU has not taken the lock since it was poked, so
  `inflight` is never larger than the true set.

Suggestion: say in run.c's comment that `poked` means "has not entered since
it was poked", and that correctness after the fast path and `stays` relies on
the pending interrupt. Optionally, have the model's test leave such a
"pending without the flag" CPU to choose later.

## Checked and found sound

- **Missed wake-ups (item 1).** Each queued thread is claimed at most once,
  and both loops leave out claimed threads. A CPU that isn't in flight is asked
  only if its choice, with claimed threads left out, is not its own thread.
  The choice it then actually makes (`best_queued`, with nothing left out) is
  at least as good, so it also differs from its own thread: no interrupt is
  wasted.

  Every pick in run.c (`schedule`, and `idle` both after a failed pick and
  after a successful one returns to `schedule`) is followed by `poke_others`.
  That call runs `joist_sched_pokes` whenever the flag is set or anything is
  in flight. Every state change that isn't a pick sets `K.resched`: ticks,
  `joist__off_cpu`, the new preemption flag, and operations. The fast path and
  `stays` change no CPU's choice (F6).

  I also ran a stress version of both property tests: 2–8 CPUs, priorities
  1–3, 60 000 burst states × 12 delivery orders, and 2 000 000 operations with
  random partial delivery. Every interrupt arrived and every CPU settled
  (`mut/stress`).
- **Livelock (item 2).** The new `k->resched = 1` (sched.c:540) is set only
  when a CPU replaces a thread that can still run with one that strictly
  precedes it in the (priority, place) order. Each place (`ready_seq`) is
  unique: it is only assigned fresh or moved between threads, never
  duplicated (ipc.c:368-369). So between outside events, the sorted list of
  running threads strictly improves with every preemption, and chains are
  finite (at most about one per CPU per new thread). The fast path's
  `joist_sched_on` replaces a caller that is blocked by then (`joist_sched_handed`
  requires BLOCKED_REPLY/RECV), so it never sets the flag that the fast path
  then clears. The bound added to `test_runq.c`'s `poke()` is a sensible limit
  that catches a loop.
- **Indices (item 3).**
  - Each CPU below `ncpus` is visited exactly once across the in-flight, idle
    and busy loops, and `me` is excluded. So `n` and `m` are at most
    `ncpus - 1`, which is below `JOIST_MAX_CPUS` (the size of `cl` and
    `order`).
  - `ncpus` is clamped to `JOIST_MAX_CPUS`, and `RUN_MAX_CPUS <= JOIST_MAX_CPUS
    <= 32` is checked by static asserts (run.c:101, sched.c:711). So the
    shifts `1u << o` and run.c's `<< i` stay in range.
  - `rq_first_unclaimed` guards `x <= JOIST_MAX_THREADS` and passes over at
    most `n` claimed threads in all queues together.
  - `choice` checks `mine <= JOIST_MAX_THREADS` and `can_run` before reading
    `run_sc - 1`.
  - `goes_later` is called only on busy CPUs; it relies on `on_cpu` being
    valid (I13).
  - The scan fallback skips threads on CPUs and claimed threads, so it agrees
    with `best_for`.
- **The CPU mask in run.c.** Bits at or above `ncpus` are never set. Because
  `joist_sched_pokes` already leaves out CPUs in flight, dropping the old
  `!o->poked` check sends no second interrupt.
- **The other listed mutations.** `pokes-herd`, `pokes-inflight-ignored`,
  `pokes-inflight-unclaimed`, `pokes-busy-in-cpu-order`, `pokes-never-preempt`
  and `preempt-no-resched` are all caught by the SMP tests. The
  `MUTATE_TIMEOUT` handling in mutate.sh (`timeout` exit 124 → TIMEOUT, which
  fails the check) is correct.
