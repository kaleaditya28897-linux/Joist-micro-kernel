# Fault injection: Phase 3's exit criterion "fault-injected client/server tests pass"

2026-10-15, P53 / build VM, in `~/joist-faultinj` (a copy of `~/joist`).
Result: **built, and it passes. No kernel or core bug found.** The test
catches all six bugs I put into the core by hand (below), so passing means
something.

## What was built

| File | What |
|---|---|
| `user/faultinj/faultinj.c` | The root task: seeded rounds, the kill points, the checks, the counts |
| `user/fiworker/fiworker.c` | The one peer program, loaded as every client and server task. Each thread carries out the root's commands (Call, Recv, Reply, reply_save, reply_saved, cap_delete, nosenders_next, bind_nosenders, recv_slots, a run of Signals, a Wait loop, exit) one at a time and writes back what each call returned |
| `user/lib/fi.h` | The page the root shares with each peer: a record per thread (command, arguments, posted/started/done counts, result, values) and the peers' slot layout |
| `user/lib/joist.h` | Wrappers the test needed that weren't there: `j_reply_save`, `j_reply_saved`, `j_signal`, `j_wait`, `j_bind_nosenders`, `j_nosenders_next`, `j_request_deadname`, `j_task_descendant`. Nothing existing changed |
| `user/Makefile`, `user/README.md` | The two programs and `lib/fi.h` added |
| `kernel/test/selftest.sh` | A `faultinj-SEED` case (one user build per seed) and seven runs |

Nothing in `kernel/` (apart from `selftest.sh`), `kernel/core`, `model/` or
`docs/` was changed.

### How a round goes

Every round builds a small world of tasks, each loaded from `fiworker` with
threads on scheduling contexts of their own (servers at priority 90,
clients at 100, a waiter at 110, the root above them at 128). The root
hands each task the capabilities the scenario needs, in fixed slots
(`fi.h`). Then it drives the threads command by command to a kill point and
kills one of them in one of the ABI's ways:

- `thread_kill`;
- deleting the last capability to the thread;
- `task_destroy`;
- deleting the last capability to the task;
- where that is the point of the case: deleting a bound reply object, or
  deleting a capability while it is in transit.

After the kill it checks what the survivors get. Before killing it waits a
random time of the seed's choosing: none, a sleep, a spin (other CPUs run
meanwhile), or long enough for everything to settle. After a task-level
kill, the seed also decides whether the root finishes the destruction with
`continue` or leaves it to the kernel.

The five scenarios run in turn, one per round. Each scenario chooses its
kill point and kill method from the seed:

1. **call**: server S, a client C (the victim), and a bystander client B.
   S binds no-senders to a notification of the root's.
2. **server**: S, client C1 (being served), and client C2 queued behind it.
   The root holds a SEND to the endpoint and a dead-name registration on it.
   In a third of rounds S is passive: it is started by the handshake
   docs/abi.md describes, then runs on C1's time.
3. **saved**: S has two threads, T0 (receives and saves the reply) and T1
   (answers through the reply object), and one client.
4. **transfer**: S, and a client with two threads, T0 (calls) and T1
   (helps). The client holds the only copy of a badged SEND to an endpoint
   of the root's. The root watches that endpoint's no-senders.
5. **signal**: signaller G holds 16 badged SENDs to one notification, one
   bit each, and signals them in order. Waiter W Waits in a loop and ORs
   together what it gets.

**Nothing hangs.** Every wait of the root's is bounded:

- polling a thread's result, 3 s;
- a notification, by `Wait` with a timeout;
- "must not happen" checks, a 20 ms watch.

A survivor that hasn't got what it is owed within the bound fails the
round, and the message names the hang.

At the end of each round:

- Every live thread must have finished its last command. The one exception
  is a thread the scenario left, on purpose, in a call the ABI lets block
  for ever: a `Recv` on a live endpoint. Such a thread is aborted and must
  come back with `E_ABORTED`. Any other blocked thread fails the round:
  "a thread is left blocked at the end of its round".
- Every thread is told to exit, and must then be `E_GONE`.
- Every task is destroyed, and destruction is driven to the end.
- `task_descendant(0)` must be `E_EMPTY`.
- The kernel's counters (`j_debug_counters`, from the other session's
  memory-growth work) must read exactly what they read after the first
  round: objects, threads, spaces, leaves, mappings, pending destructions,
  page-table frames and free frames. So a thread left blocked, an object
  not freed, or a destruction not finished would show.

The pages a round uses are given back with `ld_mark`/`ld_release`
(`lib/loader.h`, from the same work), and the root's slots are reused, so
any number of rounds run in the same memory.

### Seed and replay

- `UEXTRA="-DFAULTINJ_SEED=n -DFAULTINJ_ROUNDS=n -DFAULTINJ_FIRST=r"`.
  The defaults are seed 1, 100 rounds, and first round 0.
- Each round's choices are computed from the seed and the round number
  only, so any round can be replayed alone. A failure prints a line like
  this one (taken from a real failure while I was developing the test):

```
faultinj: FAILED: seed 7 round 3 (call): the kernel's counters moved: something of the round's is left
faultinj: replay: UEXTRA="-DFAULTINJ_SEED=7 -DFAULTINJ_FIRST=3 -DFAULTINJ_ROUNDS=1"
```

A failure line names the scenario, the kill point, the kill method and what
went wrong. With `make`:
`make BUILD=b ROOT_TASK=faultinj MODULES=fiworker USER_BUILD=build/x UEXTRA="..." b/joist.iso`.
On several CPUs the seed fixes the choices but not the timing. Where the
order is open, the checks accept exactly the outcomes the ABI allows for
each order (next section), and the counts show which happened.

## What it covers: kill points × outcomes, against docs/abi.md

| Kill point | What the survivors must see (docs/abi.md) |
|---|---|
| a client in a Call not yet received | The server's `Recv` (timeout 0) is `E_TIMEOUT`: the message went with its thread. The server goes on to serve the bystander (`OK`, right badge and value). No-senders for the victim's badge fires when its task goes, and not before: at a task-level kill straight away; at a thread-level kill only when the root later destroys the task. It fires exactly once: the summary bit once, the badge queued once, then `nosenders_next` returns 0 (I4). |
| a client in a Call its server holds | The server's `Reply` is `E_PEER` ("the caller stopped waiting"). The server goes on. No-senders as above. |
| a server around its Recv (killed with no wait for it to receive) | If the server had received: the caller gets `E_PEER`. If not: the caller gets `E_GONE` when the endpoint dies, and with a thread-level kill it stays queued until the task goes (I10). The dead name fires then, once. |
| a server holding a reply capability | **Thread killed:** the caller gets `E_PEER`, and it goes on, on its own time: a call with a timeout times out. The second caller stays queued (the endpoint lives: the task holds RECV). No dead name yet. **Task destroyed (now, or later by the root):** the queued caller gets `E_GONE`; the dead-name notification comes once (I5); a Call on the endpoint is `E_GONE` at once, both the root's and a survivor's; `request_deadname` on it is `E_GONE`. |
| a passive server holding a reply capability, on its caller's time | As above. The caller gets its scheduling context back ("killing a thread gives back what it borrowed"), which the timed-out call shows. |
| a server holding a reply capability it hasn't saved yet | The caller gets `E_PEER`. The dead name fires for a task-level kill only. |
| a server holding a saved reply | **The saving thread killed:** the caller stays blocked ("a reply object outlives the thread that saved into it"), the other thread's `reply_saved` is `OK`, and the caller gets the answer. **The reply object's last capability deleted:** the caller gets `E_PEER`. **The task destroyed:** the caller gets `E_PEER`, and the dead name fires once. |
| a sender with a capability in a queued Call | The message (and its capability) is gone: `Recv` gets `E_TIMEOUT`. No-senders for the passed badge fires once when the sender's task goes. A thread-level kill leaves the capability in the live task, so nothing fires until the task is destroyed. |
| a sender whose capability was delivered | The `Reply` is `E_PEER`. The delivered copy keeps the badge alive (it "counts as a sender", I4): **no** no-senders even when the sender's task is gone. It fires once when the second of the two copies goes, in either order. |
| a receiver with a capability on its way to it | **Thread killed:** the caller stays queued. **Task gone:** the caller gets `E_GONE` and the dead name fires once. The capability never left the sender: no no-senders until the sender deletes it, then once. |
| a capability deleted between its send and its delivery | No-senders fires on the delete, once: a message on its way is not a sender. At delivery the capability is skipped, as abi.md's *Capability transfer* says ("the sender's capability is gone ... since the send"): count 1, slot `0xffff` (none), nothing in the receive slot, and the Call is answered `OK`. On several CPUs the delete can come before the Call is made; the Call must then fail at its send with `E_EMPTY`, and nothing may arrive. |
| a thread while signalling | After the kill, no more signals. What arrived (the waiter's bits OR the root's final drain) is exactly the bits whose `Signal` had returned, plus possibly the one the thread had begun. Each bit arrived once (I2). The waiter wakes on a later signal (it isn't stuck). |
| a thread waiting for signals | Killed while waiting: the next 8 signals are not lost to the dead waiter. They stay pending in the word, and the root's `Wait` gets exactly them (I2). |

Counts from the seven self-test runs below (100 rounds each, 700 rounds).
They are printed by the test itself in each run's log and added up here:

| Kill point | thread_kill | thread cap deleted | task_destroy | task cap deleted | reply object deleted | capability deleted | total |
|---|---:|---:|---:|---:|---:|---:|---:|
| a client in a Call not yet received | 13 | 13 | 24 | 20 | - | - | 70 |
| a client in a Call its server holds | 14 | 14 | 18 | 24 | - | - | 70 |
| a server around its Recv | 14 | 10 | 15 | 10 | - | - | 49 |
| a server holding a reply capability | 7 | 10 | 11 | 21 | - | - | 49 |
| a passive server holding a reply capability, on its caller's time | 9 | 15 | 7 | 11 | - | - | 42 |
| a server holding a reply capability it hasn't saved yet | 5 | 13 | 13 | 9 | - | - | 40 |
| a server holding a saved reply | 17 | 22 | 21 | 20 | 20 | - | 100 |
| a sender with a capability in a queued Call | 11 | 8 | 7 | 5 | - | - | 31 |
| a sender whose capability was delivered | 8 | 9 | 11 | 5 | - | - | 33 |
| a receiver with a capability on its way to it | 9 | 6 | 10 | 14 | - | - | 39 |
| a capability deleted between its send and its delivery | - | - | - | - | - | 37 | 37 |
| a thread while signalling | 25 | 19 | 12 | 14 | - | - | 70 |
| a thread waiting for signals | 16 | 17 | 14 | 23 | - | - | 70 |
| **all** | 148 | 156 | 163 | 176 | 20 | 37 | 700 |

Every kill point is hit by every kill method that applies to it. Each run
must print "13 of 13 kill points covered", and the self-test requires that
line.

| Outcome checked | Times |
|---|---:|
| E_PEER | 323 |
| E_GONE | 291 |
| a killed caller's message withdrawn | 101 |
| a caller going on after E_PEER | 55 |
| a saved reply answered after its thread died | 39 |
| no-senders, each once | 280 |
| dead names, each once | 242 |
| a capability skipped at delivery | 37 |
| a Call failed at its send (the capability already gone) | 0 |
| signals exactly as sent | 114 |
| signals as sent, one in flight at the kill | 26 |
| threads left in a call for ever, aborted | 20 |

The kill-point counts are the seed's choices, so the same seeds give the
same table on any machine. The outcome split varies with the CPUs' timing:
the run before my second rebase had the same kill counts, with 327
`E_PEER` against 287 `E_GONE`, and 11 signals in flight.

## Self-test results

`make -C user`: builds with no warnings. I checked with a build from
scratch into an empty directory: exit 0, zero warnings, 22 programs.

`make -C kernel selftest` on the final tree (rebased onto `~/joist` as it
was at about 12:50 IST, IPC-buffer work included): **exit 0, 71 self-tests,
all ok.** The new lines:

```
selftest faultinj-1: ok (faultinj: all ok)
selftest faultinj-2: ok (faultinj: all ok)
selftest smp2-faultinj-3: ok (faultinj: all ok)
selftest smp4-faultinj-4: ok (faultinj: all ok)
selftest kvm-smp2-faultinj-5: ok (faultinj: all ok)
selftest kvm-smp4-faultinj-6: ok (faultinj: all ok)
selftest chk-kvm-smp4-faultinj-7: ok (faultinj: all ok)
```

The runs are seeds 1 to 7, 100 rounds each:

- 1 emulated CPU twice;
- 2 and 4 emulated CPUs;
- 2 and 4 real CPUs (KVM);
- the checking kernel (`-DRUN_CHECK_INVARIANTS`) on 4 real CPUs.

Each run must print the five lines `faultinj: <scenario> ok: 20 rounds`,
`faultinj: 13 of 13 kill points covered`, `faultinj: all ok` and
`root task exited with code 0`. All seven take 70 s together.

The rest of the suite, unchanged, also passed in the same run:

```
selftest 1 2 3 4 5 hello badptr ud rowrite spin sysfuzz sysrethole 7 8 9 ubsan-6 ubsan-hello
  ubsan-sysfuzz pingpong pingpong-long ubsan-pingpong sched ubsan-sched fault ubsan-fault cascade
  ubsan-cascade cascade-nopreempt pcid-fault pcid-pingpong pcid-cascade bench bench-gate smp
  smp2-smp kvm-smp2-smp smp2-hello smp2-pingpong smp2-sysfuzz smp2-fault kvm-smp2-fault smp2-sched
  smp2-cascade kvm-smp2-cascade smp4-smp kvm-smp4-smp smp4-hello smp4-pingpong smp4-sysfuzz
  smp4-fault kvm-smp4-fault smp4-sched smp4-cascade kvm-smp4-cascade chk-kvm-smp4-smp
  chk-kvm-smp4-fault chk-kvm-smp2-pingpong chk-kvm-smp4-pingpong pingpong-buf
  kvm-smp4-pingpong-buf chk-kvm-smp2-pingpong-buf grow kvm-smp4-grow40 chk-kvm-smp4-grow: ok
bench gate: ok: a system call that does nothing: median 476 cycles (baseline 384)
bench gate: ok: two address spaces, passive server: median 1372 cycles (baseline 1344)
bench gate: ok: one address space, passive server: median 920 cycles (baseline 880)
```

(The do-nothing system call is at 476 against a limit of 480. That has
nothing to do with this work, but it is close to failing.)

The complete output of one run, the checking kernel on 4 real CPUs:

```
faultinj: seed 7, rounds 0 to 99, 4 CPUs
faultinj: after the first round: objects 83, threads 1, spaces 1, leaves 2, mappings 29, pending 0, page-table frames free 1554, frames free 60359
faultinj: after the last round: objects 83, threads 1, spaces 1, leaves 2, mappings 29, pending 0, page-table frames free 1554, frames free 60359
faultinj: kill point: 11 x a client in a Call not yet received (thread_kill 1, task_destroy 7, last task capability deleted 3)
faultinj: kill point: 9 x a client in a Call its server holds (thread_kill 3, task_destroy 2, last task capability deleted 4)
faultinj: kill point: 3 x a server around its Recv (thread_kill 1, last thread capability deleted 1, task_destroy 1)
faultinj: kill point: 6 x a server holding a reply capability (thread_kill 2, last thread capability deleted 1, last task capability deleted 3)
faultinj: kill point: 11 x a passive server holding a reply capability, on its caller's time (thread_kill 4, last thread capability deleted 2, task_destroy 2, last task capability deleted 3)
faultinj: kill point: 5 x a server holding a reply capability it hasn't saved yet (last thread capability deleted 2, task_destroy 2, last task capability deleted 1)
faultinj: kill point: 15 x a server holding a saved reply (thread_kill 1, last thread capability deleted 5, task_destroy 1, last task capability deleted 4, reply object deleted 4)
faultinj: kill point: 5 x a sender with a capability in a queued Call (thread_kill 1, last thread capability deleted 2, last task capability deleted 2)
faultinj: kill point: 4 x a sender whose capability was delivered (thread_kill 1, last thread capability deleted 2, task_destroy 1)
faultinj: kill point: 5 x a receiver with a capability on its way to it (thread_kill 1, task_destroy 2, last task capability deleted 2)
faultinj: kill point: 6 x a capability deleted between its send and its delivery (capability deleted 6)
faultinj: kill point: 11 x a thread while signalling (thread_kill 4, last thread capability deleted 1, task_destroy 3, last task capability deleted 3)
faultinj: kill point: 9 x a thread waiting for signals (thread_kill 3, last thread capability deleted 2, task_destroy 3, last task capability deleted 1)
faultinj: outcome: 45 x E_PEER
faultinj: outcome: 44 x E_GONE
faultinj: outcome: 16 x a killed caller's message withdrawn
faultinj: outcome: 10 x a caller going on after E_PEER
faultinj: outcome: 6 x a saved reply answered after its thread died
faultinj: outcome: 40 x no-senders, each once
faultinj: outcome: 33 x dead names, each once
faultinj: outcome: 6 x a capability skipped at delivery
faultinj: outcome: 0 x a Call failed at its send (the capability already gone)
faultinj: outcome: 18 x signals exactly as sent
faultinj: outcome: 2 x signals as sent, one in flight at the kill
faultinj: outcome: 4 x threads left in a call for ever, aborted
faultinj: 13 of 13 kill points covered
faultinj: call ok: 20 rounds
faultinj: server ok: 20 rounds
faultinj: saved ok: 20 rounds
faultinj: transfer ok: 20 rounds
faultinj: signal ok: 20 rounds
faultinj: all ok
```

### Beyond the self-tests

- **Longer runs.** Seeds 11 to 16, 300 rounds each, on:
  - 1, 2 and 4 emulated CPUs;
  - 4 KVM CPUs;
  - the checking kernel on 4 KVM CPUs.

  That is 30 runs and 9 000 rounds; all passed, with 13 of 13 kill points
  each. Before the per-round counter check existed, seeds 1 to 6 × 300
  rounds × {1, 2, 4 emulated, 4 KVM} also passed (24 runs). A run of 300
  rounds takes 14 to 18 s, 50 s on the checking kernel. One plain KVM run
  took 35 s and one checking run 81 s, I think because the other
  session's builds were loading the VM; they passed. No run was flaky.
- **Does it catch bugs?** I broke the core six ways, one at a time, and
  ran seed 1 for 100 rounds on one CPU. Each break was temporary, in my
  copy only; I checked `kernel/core` was identical to `~/joist`'s
  afterwards. Every one was caught, at the first round that reaches it:

| Mutation (kernel/core) | Caught as |
|---|---|
| a dying thread doesn't wake the caller it owes a reply (`ipc.c`) | `round 0 (server; a server holding a reply capability; task_destroy): a caller hangs after its server died` |
| a freed reply object doesn't wake its caller (`ipc.c`) | `round 21 (saved; a server holding a saved reply; reply object deleted): a caller hangs after its saved reply's object was deleted` |
| a dying endpoint wakes its queue with E_PEER instead of E_GONE (`notify.c`) | `round 0 (server; ...; task_destroy): a caller queued on an endpoint that died didn't get E_GONE` |
| a dying endpoint doesn't signal its dead-name registrations (`notify.c`) | `round 0 (server; ...): no dead-name notification when the endpoint died` |
| a thread killed in `Wait` isn't taken off the notification's queue (`ipc.c`) | `round 13 (signal; a thread waiting for signals; last thread capability deleted): a signal was lost: sent after its waiter was killed, it didn't stay pending` |
| a capability that can't be placed is reported as delivered (`ipc.c`, `deliver`) | `round 12 (transfer; a capability deleted between its send and its delivery; capability deleted): a capability deleted after its send was delivered, or not reported as skipped` |

## Bugs found

**None in the kernel or the core.** Everything the test checks behaves as
docs/abi.md says, on 1, 2 and 4 CPUs, emulated and real, and on the
checking kernel.

The test found one failure while I was developing it, and that one was
mine:

- **What happened:** seeds 2, 3, 4 and 7 failed at their first `call`
  round with "the kernel's counters moved" (objects 53 → 56).
- **Cause:** splitting a page off the pool makes a frame object that the
  root keeps. So the object count rose the first time a round needed more
  pages than any round before it.
- **Fix:** the root now splits off as many pages as any round can take
  (48) before the first round, and every round checks it stayed within
  that.

## Not done, limits, and what to know

- **"A Call failed at its send" never happened.** This is the order where
  the capability is deleted before the Call is made, on several CPUs. The
  client is given time to be in its Call before the delete, so this order
  was never observed in any run. The check for it exists but has never
  run.
- **"Random points" are random among the protocol's states**, plus a
  random delay before the kill. They are not arbitrary instructions:
  - On one CPU, a delay lets the peers run until they block.
  - On several CPUs, the kills land while the peers run, which is how
    kills mid-entry to the kernel are reached ("whatever it was in the
    middle of entering the kernel for is undone": abi.md, *More than one
    CPU*).
  - The counts show both orders happen: a server killed around its Recv
    gives `E_PEER` or `E_GONE` in different shares on 1 and 4 CPUs, and a
    signal in flight at the kill happens 2 to 26 times per 700 rounds.
- **"Must not happen yet" checks watch for 20 ms.** For example: a queued
  caller staying blocked while its endpoint lives, or a saved reply's
  caller staying blocked after the saving thread dies. A wrong wake later
  than 20 ms would not be caught there, though it would almost always fail
  the next check (e.g. a wrong result code at the end).
- **Not covered:**
  - `task_reap` as a kill (it needs the task's id; `task_destroy` and the
    last capability are covered);
  - `thread_suspend` and `thread_abort` as faults: abort is used only at
    teardown;
  - faults (fault IPC) as a way to die;
  - two capabilities in one message;
  - receive slots absent or occupied (a skipped delivery is covered via a
    deleted capability);
  - the IPC buffer's words in a message whose sender dies (the buffer
    arrived in `~/joist` while I worked; the test sends no words);
  - passive servers in the saved-reply and transfer scenarios (only in
    the server scenario);
  - a server's `ReplyRecv` replying to a dead caller.
- **The patch depends on the other session's work in progress.** It was
  made against `~/joist` as of about 12:50 IST today. That tree has
  `growroot` (its `ld_mark`/`ld_release` in `lib/loader.h`, which
  faultinj uses, and `j_debug_counters`) and the IPC-buffer work. I first
  built on an older copy with my own page recycling in `loader.h`. When
  theirs appeared, I rebased onto it and dropped mine. When the IPC buffer
  arrived, I rebased again, re-ran `make -C user` and the full
  `make selftest` (both above), and checked the patch with
  `git apply -p1 --check` against that tree's files: it applies, and the
  result is byte-identical to the tested tree.
- **Overlap with the other session.** If they also add Signal/Wait or
  no-senders wrappers to `lib/joist.h`, the names will clash: mine are
  `j_signal`, `j_wait`, `j_reply_save`, `j_reply_saved`,
  `j_bind_nosenders`, `j_nosenders_next`, `j_request_deadname` and
  `j_task_descendant`.
- **The patch is not the literal `diff -ruN joist joist-faultinj`.**
  `~/joist` kept changing while I worked, so that command would also
  revert their newest changes. `patch.diff` was made the same way, but
  from a copy of the five shared files as they were in `~/joist` when I
  last rebased, to those files with my changes, plus the three new files.
  The paths are `joist/...` and `joist-faultinj/...`, so `git apply -p1`
  works at the top of the tree. It contains only my changes.
- **One stray file outside `~/joist-faultinj`.** I once wrote a make log
  to `/tmp/fi-make.out` on the VM, and deleted it straight after.
