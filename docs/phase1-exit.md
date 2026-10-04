# Phase 1 exit

**Status: accepted by the project owner, 2026-10-04**, with the optional TLA+
model to be done as well (work item 8, below). Phase 1 (§11, *Semantic model
on the host*) is closed. It checks each exit
criterion and activity against its evidence and lists what carries forward.
Every number cited is in `docs/phase1-data/`, with provenance in that
directory's README; the work itself is in `docs/phase1-plan.md`, *Progress*.

## Exit criteria

| Criterion (§11) | Met by |
|---|---|
| Invariants proven or property-tested | All ten (I1–I10) property-tested: five per-area property tests against independent shadow models, and a whole-model test over all 25 operations checking every invariant after every step (200 seeds × 2 000 steps each), plus libFuzzer over the same executor. I8, I2, I6 are also checked directly per operation, and I9 as a measured work bound. `make mutate`: 43 of 43 injected bugs caught; `make fuzz-sanity`: 7 of 7 |
| The API written down as `docs/abi.md` | Written from the model. A tool (`tools/phase1/abicheck.py`) checks it both ways against the tests: all 139 result codes it lists for its 21 operations are produced by a unit or property test, and no code a test reaches is missing from it (`phase1-data/item7/abi-coverage.csv`) |

Nothing is *proven*: the invariants are property-tested, which §11 accepts.

## Activities

| Activity (§11, `phase1-plan.md`) | Status | Where |
|---|---|---|
| Host-side C model: tasks, threads, capabilities, endpoints, badges, notifications, lifetime notifications | done, plus reply objects (deferred replies) and queued no-senders, which item 6 showed are needed | `model/`, work items 1–4, 6 |
| Library under the host harness with all sanitizers | done: `make check` runs every test under plain, ASan, UBSan, MSan, TSan, and a small-capacity build; `make sanity` proves each sanitizer is live | `model/Makefile` |
| Invariant tests | done (above) | work items 1–5 |
| libFuzzer over the model | done, with a seed corpus recorded from the property test and `make fuzz-sanity` | work item 5 |
| Settle 0001's five parameters | done: amendment accepted 2026-10-03 (4 registers + 4 KiB IPC buffer; 64-bit server-chosen badges; 64-bit notification word; two-level capability table; no bare `Send`) | `decisions/0001`, work item 6 |
| `docs/abi.md` | done | work item 7 |
| Optional TLA+ model of IPC and capability deletion | done 2026-10-04, after acceptance, at the owner's request: IPC racing a preemptible destruction; all properties hold given one new rule (a dying space refuses capabilities), which TLC found | work item 8, `model/tla/` |

## Found by the model, beyond the plan

- **No-senders can't be delivered as notification bits** once badges are
  64-bit values: two clients going at once merge. Now queued through the
  preallocated accounts and collected by the server (0001 amendment, item 2).
- **Deferred replies are required** (`proc_wait`, `select`, terminal reads),
  and they, not a bare `Send`, are what the Hurd's one-way messages are for.
  Now reply objects (0001 amendment, item 5).
- **Task cycles aren't collected**: tasks holding each other's capabilities
  outlive their last outside capability, stranding their quota
  (work item 5). **Open, for a decision** (below).
- **The fuzzer needed help to find anything**: four changes before it caught
  the injected bugs, and a seed corpus before it caught them reliably
  (work item 5).

## Carried forward

Open, in the order they block work (`docs/abi.md`, *Open*):

- **Before Phase 3 builds on the ABI:**
  - **Task cycles** (owner's decision): parents destroy descendants through
    the recorded parent chain, or creators always keep a capability.
  - **Capability transfer in messages**: count, rights masking, failure
    behaviour. The model uses an internal stand-in.
  - **Preemptible cascades** (I9, 0003): a cascade is bounded by the tables
    (measured about 30× the largest non-cascading operation) but not per
    operation. The kernel needs a preemptible version, with the model's
    work list as its continuation.
- **Phase 3:** revoke and 0011's batched capability copy, not yet in the
  model; thread state (0010) and fault IPC (0009), outside the model's scope.
- **§8.1 tooling** (`phase1-plan.md`, *Tooling*): the TLA+ tools and CBMC
  are installed (2026-10-04, user-local). `clang-tidy`, `scan-build` and
  `ld.lld`/`llvm-*` need root: one `xbps-install` for the project owner.

Phase 0's carry-overs for Phases 4–10 are unchanged (`docs/phase0-exit.md`).

## The model at exit

| | |
|---|---|
| Source | `model/src/`: 4 files, kernel rules (no `malloc`, no recursion, no VLAs, bounded loops) |
| Tests | 77 tests (unit and property); `make check` runs each in six builds |
| Mutations | 43 in `model/test/mutations.txt`, all caught |
| Fuzzing | `make fuzz` from a 32-sequence seed corpus; longest run 15 minutes, 6.6 million steps, no crash |
| Pushed to | `pvehome` (`git@<pvehome-address>:/srv/git/Joist.git`), branch `main` |
