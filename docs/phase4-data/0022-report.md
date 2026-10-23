# 0022: revoke by kill and the batched capability copy — implementation report

Tree: `~/joist-p4b` on the build VM, base commit `5d242a0` ("base").
Patch: `0022.patch` (`git diff 5d242a0` of the tree, new files included).
Logs: `model-check.log`, `mutate.log` (the final run), `mutate-first.log`
(the first run, before the fix below), `kernel-check.log`.

## What was built

### Core (`kernel/core/`, `kernel/include/joist/core.h`)

- **`JOIST_R_KILL`** (= `JOIST_R_SEND_ONCE`, 0x4), commented as the kill
  right on frames (and regions when they exist).
  `full_rights(JOIST_OBJ_FRAME)` now includes it, so I8's check accepts it.
  Boot frames get it (`joist_boot_frame`); `frame_split` already copies the
  rights it had, so a split keeps KILL, or keeps lacking it (tested).
- **`joist_obj_kill(k, cs, slot)`** (cspace.c). Checks in order:
  `E_RANGE`/`E_DEAD` (space), `E_RANGE`, `E_EMPTY`, `E_TYPE` (not a type that
  can be killed), `E_RIGHTS` (no KILL), `E_GONE` (killed already). Then
  `o->dead = 1`, the field endpoints, tasks and threads already die by, and
  `joist__frame_freed(k, obj)` to unmap it everywhere, with a `TODO(0020)`
  saying the sweep replaces it. A `TODO(0021)` comment marks where the
  lent-frame `E_STATE` check goes, before anything changes. There is no code
  for it. Which types can be killed is a `killable()` switch that returns 1
  for frames only, and the effect is a per-type switch, so adding regions
  means adding one case to each.
- **Every use of a frame fails `E_GONE`.** I found the uses with grep:
  `map` and `frame_split` both go through `joist__control_cap` (sched.c),
  which already returns `E_GONE` for a dead object, so neither map.c nor the
  mapping calls change. `obj_kill` itself returns `E_GONE` too.
  `boot_frame`'s overlap scan still counts a dead frame's pages, so no page
  is in two frames (I12). `cap_read` still reads the capability (as for dead
  endpoints). Derive, give, transfer in a message and delete still work on a
  dead frame's capabilities, as they do on dead endpoints'. A copy of a dead
  frame is dead.
- **Lifetime and quota.** These are unchanged: the object, its pages and its
  donor's unit go with the last capability (`obj_unref`). I1 holds as it does
  for dead endpoints.
- **Invariant checker.** The frame branch of `joist__check_notify` no longer
  rejects `dead` on a frame. `joist_check_invariants` (cspace.c, in the loop
  that already walks `k->map` for charges) adds "0022: killed frame N still
  mapped". I put it there and not in map.c because I was told not to touch
  map.c, and because that loop already reads `k->map`, which 0020 removes;
  its comment says a dying frame may stay mapped until the sweep finishes.
- **`joist_cap_copy_batch(k, cs, task_slot, first, count, mask, flags,
  *done, *skipped)`** (task.c, next to `cap_give`), with
  `JOIST_COPY_BATCH` = 32 and `JOIST_COPY_RECV` = 1. Check order:
  `E_RANGE`/`E_DEAD` (caller), the task capability (`E_RANGE`, `E_EMPTY`,
  `E_TYPE`, `E_RIGHTS` for CONTROL), `E_DEAD` (child dying or destroyed),
  then `E_RANGE` (count > 32, the range past the table, or an unknown flag).
  Then for each slot: empty slots are passed over; slots without GRANT, and
  endpoint copies that would carry RECV, are skipped and get their bit set;
  the copy stops with `E_RANGE` (missing target leaf) or `E_OCCUPIED`. The
  copy itself is `joist__cap_grant`, which is exactly `cap_give`'s copy: one
  `refs++` and `joist__cap_added` (RECV count, the badge account's
  `send_caps++`). Like derive and give, it never allocates or fires
  anything; `copy_cap`'s only allocation is `acct_reserve`, and only `mint`
  calls it. No-senders then waits for the child's copies (tested). The work
  is 32 × (1 + `JOIST_MAX_BADGE_ACCTS`) at most (tested).

### Kernel and user side

- `kernel/sys/abi.h`: `ABI_CAP_COPY_BATCH` 28 and `ABI_OBJ_KILL` 29.
  `abi.c` decodes them: the batch's fifth argument (flags) comes in `R8`,
  `done` goes back in `RDX` and `skipped` in `R8`, as `cap_read` returns its
  badge in R8. `run.c` needs nothing: it has no per-call table, and
  `cap_give` is wired in abi.c alone.
- `user/lib/joist.h`: `J_R_KILL`, `J_COPY_BATCH`, `J_COPY_RECV`,
  `j_cap_copy_batch(task, first, count, mask, flags, &done, &skipped)` and
  `j_obj_kill(slot)`. `kernel/test/test_abi_user.c` checks them against the
  kernel's at compile time.
- `kernel/test/test_abi.c`: `abi_cap_copy_batch_and_obj_kill`, which tests
  the registers in and out. `kernel/fuzz/fuzz_abi.c` knows both numbers.
- **`user/revokeroot`** (new): a root task that runs both calls through the
  real ABI in QEMU. It checks done and skipped, a stop part way, 33, an
  unknown flag, `COPY_RECV`, and a batch into a destroyed task. It also maps
  a frame into itself and into a child, kills it, and checks with
  `debug_counters` that both mappings are gone. After the kill, map, split
  and kill fail `E_GONE`, and all objects come back. `selftest.sh` runs it
  emulated, on 2 real CPUs (KVM) and on the checking kernel.

### Docs (`docs/abi.md`)

- Objects table: frames are created with KILL. *Lifetime*: frames die when
  killed, and a dead object's slots and quota behave as they do for dead
  endpoints.
- Rights table: a KILL row.
- Capability table: a `cap_copy_batch` row. Frames table: an `obj_kill` row.
  Both use the house style: arguments, results in check order, effect,
  bound.
- I12's wording now says "unkilled". Encoding: 20-29, with R8 used both ways
  by the batch. `E_GONE`'s meaning now includes frames. Open item 4 is
  marked settled.

### Model tests

- `model/test/test_kill_copy.c`: 18 unit tests, 6 for kill and 11 for the
  batch, a combined "hand out without KILL, then revoke" test, and a checker
  test.
- `model/test/test_props_kill_copy.c`: a property test against its own
  shadow. The shadow covers tasks made by the root (with cascading
  destruction), objects, derive, mint, delete, leaves, boot frames, split,
  map, unmap, kill and batch. After every step it compares the result (and
  the batch's done and skipped values) and the whole state: every slot of
  both leaves of every space, quota, charges, task objects, every object's
  liveness, type, death, references, donor and pages, and every mapping by
  index. Then `joist_check_invariants` must pass. The defaults are 100 seeds
  × 1500 steps. The coverage from one run is in the log, for example kill
  OK=1644 (536 of which unmapped something), batch OK=3632, stopped part way
  3998 times, E_OCCUPIED=2729, E_DEAD=11396.
- `model/test/exec.c`/`exec.h` (the whole-model executor, which
  `test_props_all` and libFuzzer drive): `JX_OBJ_KILL` (weight 2) and
  `JX_COPY_BATCH` (4). Their own checks: a killed frame is dead and mapped
  nowhere; each copy the batch reports is the source's, masked, with the same
  badge and slot; a refusal reports nothing done; `done` ≤ count, and =
  count on success. These run with preemption a third of the time, so
  batches meet half-destroyed children, with every invariant checked
  (TLA+'s SpaceEmptied included). The fuzzer learns the calls through this
  executor. Corpora recorded before the change decode differently, because
  the weight total changed.
- `test_map.c`: the boot-frame rights check now includes KILL.

### CBMC

`model/cbmc/harness_cspace.c` gets op 9 (`obj_kill`) and op 10
(`cap_copy_batch`, with an I8 assertion for every copy). They are **not** in
`CBMC_OPS`, and the non-inductive mode still picks from ops 0-8; run them with
`make -C model cbmc CBMC_OPS="9 10"`. CBMC results: see *Results*.

## Choices the decision left open, and what I chose

1. **A dying child is `E_DEAD`, unlike `cap_give`'s `E_GONE`.** A child's
   task object dies at the moment its destruction begins (0016). So
   "dying" always shows up as a dead task capability, and with today's model
   no state has a dying space behind a live task object. I turn the task
   capability's `E_GONE` into `E_DEAD`, as the decision says, for dying and
   destroyed children alike. That matches the result-code table ("the
   capability space has been destroyed"). `cap_give` still says `E_GONE`. If
   you prefer one code for both calls, that is a one-line change.
2. **What counts as a "receive right":** an endpoint capability whose copy
   would carry RECV, so the test applies *after* the mask. A mask without
   RECV therefore makes a send-only copy of a receive capability, which is
   copied. A notification's RECV (wait) is not treated as a receive right.
   Both choices are tested. The alternative is to test the source's rights
   and skip whenever the source has RECV.
3. **Targets are checked only where a copy would land.** An empty or skipped
   source slot never stops the batch, even if the child's slot is occupied
   or its leaf is missing. An absent leaf on the *caller's* side means
   nothing to copy, not `E_RANGE`.
4. **`done`** is the number of slots dealt with before the stop, so the
   caller resumes at `first + done`. It is 0 on a refusal, and both outputs
   are written on every path. `skipped` bits are only set below `done`.
   `count` = 0 succeeds with done = 0.
5. **Argument checks come after the task capability** (count, range, flags →
   `E_RANGE`), following `cap_give`'s order (task, then slots). Unknown
   flags are `E_RANGE`.
6. **Copies are made as they are**, including capabilities to dead
   endpoints or dead frames (Mach copies dead names as dead names) and the
   child's own task capability.
7. **Kill needs KILL only, not CONTROL.** A second kill is `E_GONE` ("every
   capability fails `E_GONE`"), not a silent OK. `E_TYPE` is checked before
   `E_RIGHTS`, because 0x4 means SEND_ONCE on other types.
8. **System call numbers 28 and 29**, in the capability range rather than
   the frames range (40s), which the other session's map calls are likely
   to fill.
9. **`JOIST_COPY_BATCH` is its own constant** (32) and is not tied to
   `RUN_PREEMPT_SLOTS` at compile time. `cascade-nopreempt` builds with
   `RUN_PREEMPT_SLOTS=0`, which would break an equality assertion.

## Things the other session should know when applying

- **map.c is touched in one line.** I was asked to give boot frames the
  kill right in `joist_boot_frame`, but I was also told not to touch map.c,
  and the rights are written there. The patch changes only the rights
  expression on `joist_boot_frame`'s `new_frame` call (adds
  `JOIST_R_KILL`). Apply with `git apply --exclude=kernel/core/map.c` and
  make that one change by hand in your new map.c. The mutation
  `kill-boot-frames-lack-right` anchors on that text.
- The checker line "killed frame still mapped" reads `k->map` (cspace.c),
  as the charge count beside it does. When 0020 removes the table, it
  becomes "a killed frame is dying, and its map count reaches 0 when its
  sweep ends".
- `test_props_kill_copy.c` shadows `map`/`unmap` as they are today (whole
  frames, one va per frame). Its `sh_map`/`sh_unmap_call` and the per-index
  mapping comparison will need 0020's semantics, and so will exec.c's kill
  check, which scans `k->map`.
- 0021: if `space_donate` takes its frame through `joist__control_cap`, a
  killed frame is already refused with `E_GONE` ("a killed frame can't be
  lent"). The lent check in `obj_kill` goes at the `TODO(0021)`.
- TLA+ is **not** extended (the decision's *Consequences* asks for "TLA+'s
  dying-space rule extended to the batch"). That was outside this task's
  list. The C side enforces the rule, and the executor's preempted runs
  check it against SpaceEmptied.

## Results

RESULTS_PLACEHOLDER

## Where I think the decision is wrong or underspecified

1. **Revoke takes back the authority but not the memory.** A killed frame's
   pages stay its own until its *last* holder deletes its capability.
   `boot_frame`/I12 forbid a second frame over the same pages, and nothing
   else makes frames. So `mm` can't reuse the memory it revoked while any
   client, maybe a hostile one, still holds a dead copy. The decision counts
   the quota unit as the exposure, but the pages are the bigger one. Options:
   (a) when the kill runs, move the pages back to a frame the killer
   names (the dead object keeps only its identity); (b) accept it, and have
   `mm` delete-notify the holders (but frames have no no-senders). This
   needs deciding before `mm` relies on revoke for memory.
2. **`E_DEAD` for a dying child can't be reached as a separate case** (see
   choice 1). The decision reads as if a dying child had a live task
   capability. It doesn't: the mutation "dropping the dying-child check" is
   caught because the batch then copies into a dead or dying space, not
   because of a distinct code path.
3. **"Receive rights"** has no exact meaning for Joist, where one endpoint
   capability can hold SEND and RECV together. Choice 2 is one reading.
4. **O(1) kill** is O(1) only once 0020's sweep exists. Today the unmapping
   scans the mapping table inside the call (bounded by
   `JOIST_MAX_MAPPINGS`), which is what the ABI row's bound says.
5. **KILL shares the SEND_ONCE bit.** That is fine while SEND_ONCE is
   unused, but if it is ever given meaning on endpoints, "mask away KILL"
   with a type-blind mask (`~0x4`, as `fork` would pass for every slot)
   would also strip it from endpoints. That is harmless today.
6. The decision says `count ≤ 32` is "the size of one destruction step
   (`RUN_PREEMPT_SLOTS`)". That link is not enforced (choice 9). Each copy
   also costs up to `JOIST_MAX_BADGE_ACCTS` steps (the account lookup), so
   one batch costs about 32 × (1 + accounts), more than one sweep step of 32
   slots.

## Hurd glibc's fork: does it stop the parent's other threads?

**I could not read the source.** The Hurd testbed VM isn't running (nothing
listens on 2223, and I did not start it). There is no glibc tree on the P53
or the build VM, and I did not fetch one. Note that in current glibc the file
is `sysdeps/mach/hurd/_Fork.c`, not `fork.c` (renamed in 2.34).

There is **indirect evidence in this repository**, from phase 0's nested-
function survey of glibc 2.43:

- `docs/phase0-data/nested-functions.csv:266` and `nested-captures.csv:215`:
  `sysdeps/mach/hurd/_Fork.c`, line 92, a nested function `resume_threads`
  that captures `i; nthreads; ss; stopped; thread; threads`;
- `docs/phase0-data/amd64/clang-glibc-root-causes.csv:531`: it is called at
  line 427.

A function that resumes a `threads` array guarded by a `stopped` flag
implies they were suspended earlier in `_Fork`. Its definition at line 92 and
its call at line 427 bracket the body where port names are recreated. That
fits my memory of the code: it suspends every thread except itself and the
signal thread before `task_create`, and resumes them after the child is set
up. So the answer is **probably yes**, but this is inference, not a reading
of the source. Check it on the testbed before step 3 relies on it.
