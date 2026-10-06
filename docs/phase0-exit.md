# Phase 0 exit

Phase 0 (§11, *Study and inventory*) closed on **2026-10-01**. This file checks
each exit criterion and each Phase 0 activity against its evidence, and lists
what carries forward. Every number cited lives in `docs/phase0-data/`, with
its provenance in that directory's README.

## Exit criteria

| Criterion (§11) | Met by |
|---|---|
| A written inventory | `docs/mach-inventory.md`: every Mach call the Hurd uses, by component, measured on i386 and x86_64 and traced through a complete package build; static cross-reference of all 196 calls; traps; OOL parameters; port-set call sites |
| A decision-log entry for every row in §6 | `docs/decisions/` 0001 and 0004–0013 cover all 16 rows (index: `docs/decisions/README.md`, *§6 coverage*); 0014 covers what §6 never listed (futexes, yield, queue limits) |
| `docs/client-recovery.md` exists with no case "undecided" | every case decided and every **(confirm)** marker checked (§8); §4.1 decides where description state lives |
| Decisions 0001–0003 written | written, and accepted 2026-10-01 |

**All decisions 0001–0014 were accepted by the project owner on 2026-10-01.**

## Activities

| Activity (§11) | Status | Where |
|---|---|---|
| Run Debian GNU/Hurd x86_64 in QEMU | done: VM 111 on pvehome | `docs/testbed.md` |
| Build the Hurd, GNU Mach and glibc from source | done, all clean with GCC on x86_64 | `phase0-data/README.md` stages 11–13 |
| Build GNU Mach, the Hurd, MIG output and glibc with Clang; record every failure | done | `docs/clang-inventory.md` |
| `rpctrace` on boot, shell and compile | shell and compile complete (after fixing rpctrace's `gsync_wait` deadlock); boot partial, see carry-overs | `phase0-data/README.md` stages 1, 8, 14 |
| Mark every `.defs` call used / unused / used-by-one-thing | done | `mach-inventory.md` *Static results* |
| Mach dependency inventory with planned mapping | done | `mach-inventory.md` |
| Inventory file-descriptor state | done | `client-recovery.md` §4.1, §8 |
| `client-recovery.md` and a first idempotency table | done | `client-recovery.md`, `rpc-idempotency.md` |
| Size Clang/glibc against a fixed budget (0002) | done: Clang branch accepted | decision 0002 |
| Read Minix 3 RS/DS, QNX Neutrino, seL4/L4 IPC, Hurd-on-L4 post-mortems | the project owner's reading; not tracked in the repository | — |

## Carried forward

Open items inside accepted decisions, in the order they block work:

- **Phase 3:** 0001's open items: message registers, badge width,
  **notification bit-set width** (which 0005's `select` protocol and 0014's
  futexes both depend on), capability-space representation (with 0011's
  capability-copy loop), and whether a bare `Send` is needed.
  *Later status: all five were settled in Phase 1 by the accepted amendment
  to 0001 (2026-10-03; `docs/phase1-plan.md`, work item 6): 4 message
  registers plus a 4 KiB IPC buffer, 64-bit badges, a 64-bit notification
  word, a two-level capability table, no bare `Send`. 0011's capability copy
  is still open, now carried by `docs/phase2-exit.md` to Phase 3.*
- **Phase 4:** 0007's fault-reply read-ahead and 0008's delegated-range
  escape hatch, which glibc will be written against.
- **Phase 5:** the `io_select` readiness-notification change in every server
  that implements it (0005); the OOL transfer modes in the MIG backend (0006).
- **Phase 7:** 0010's TLS-base mechanism (`set_tls_base` or `FSGSBASE`); the
  glibc futex layer and per-thread notification (0014); the first
  Linux→Hurd cross-build of glibc with Clang, which is 0002's first
  condition.
- **Phase 10:** 0012's network packet path (today a deep Mach message queue).

Known gaps in the Phase 0 evidence, each recorded where it applies:

- **Boot under `rpctrace`:** the translator start-up handshake (`fsys_startup`)
  never completes when traced, so the boot column of the inventory is
  partial. Untraced, the workload works; two of three rpctrace defects are
  fixed (`tools/phase0/rpctrace-gsync.patch`,
  `tools/phase0/rpctrace-null-receive.patch`) (`phase0-data/README.md`
  stage 14).
- **libports' `thread_switch` in long-running servers** is unmeasured: those
  daemons were started before the instrumented libc (stage 15).
- **Unlinked-but-open across processes** is not detectable from the traces
  (`client-recovery.md` §8 Q2).
- **0002's upstream report** of the clang-on-Hurd hang is to be filed; the
  reproduction material is committed.

## The testbed at exit

VM 111 runs the stock Debian packages: `libc0.3` 2.43-6 (byte-identical to the
archive's package, `dpkg --verify` clean after the temporary instrumented
libc) and `hurd` 0.9.git20260919-1. It has no snapshots. The patched tools
(`rpctrace` with both patches) and the build trees are under `~aditya/spike`,
and the raw traces whose checksums `phase0-data/` records are under
`~aditya/trace` and `~aditya/trace2`.
