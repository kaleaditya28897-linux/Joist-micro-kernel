# Joist

**Joist carries the GNU Hurd.** It is a microkernel in plain C (C17, built
with Clang) written to replace GNU Mach underneath the GNU Hurd, reusing as
much of the Hurd's userland as its goals allow:

1. **Reliable**, as QNX is: bounded kernel work, no kernel memory a task can
   exhaust, priority inheritance through synchronous IPC.
2. **Resilient**, as Minix 3 is: every server has a crash and restart story.
3. **POSIX**: real software running unmodified on glibc.
4. **Reuse of the Hurd**, where 1-3 allow; **performance** last.

Kept simple enough for one person to read all of it. The full plan, its
reasoning and its phases are in [`joist-microkernel-plan.md`](joist-microkernel-plan.md).

## Status

Work in progress, phase by phase; each phase has a plan and an exit record
in [`docs/`](docs).

| Phase | What | State |
|---|---|---|
| 0 | Study and inventory: what the Hurd uses of Mach, measured | done ([exit](docs/phase0-exit.md)) |
| 1 | Semantic model on the host: capabilities, IPC, tasks; property tests, CBMC, TLA+ | done ([exit](docs/phase1-exit.md)) |
| 2 | Kernel bootstrap on x86-64 (Limine) | done ([exit](docs/phase2-exit.md)) |
| 3 | IPC and capabilities in the kernel: the model as its object layer, scheduling contexts, SMP | done ([exit](docs/phase3-exit.md)) |
| 4 | Memory management: frames, page tables, the `mm` server, copy-on-write, revocation | done ([exit](docs/phase4-exit.md)) |
| 5 | MIG retarget and libports, with the region capability, per-tree limits and glibc's `sysdeps/joist` | in progress ([plan](docs/phase5-plan.md)) |
| 6-13 | Paging and libpager, the glibc port, bootstrapping the Hurd, resilience, drivers, server hardening, POSIX conformance, native optimisation | planned ([plan](joist-microkernel-plan.md) §11) |

### Where it stands (2026-11-15)

**The foundation is built.** A kernel core whose invariants are checked by
property tests, CBMC proofs and mutation testing; synchronous IPC with
capabilities and lifetime notifications; scheduling contexts and SMP;
memory managed by a user-level server, `mm`, with copy-on-write `fork`;
and fault-injection tests of all of it.

**Phase 5 is bringing the Hurd's own code across.** Done so far:

- the region capability: a task maps its own pages, in a range `mm` gives it;
- per-tree memory limits in `mm`;
- MIG retargeted to Joist's IPC, with the `.defs` files unchanged;
- glibc's `sysdeps/joist`, built with Clang: about 1 900 of glibc's own
  tests pass on Joist, threads (htl), `malloc` and the Mach layer among them;
- the Hurd's libports, its request loops and no-senders on Joist,
  tested under fault injection on 1, 2 and 4 CPUs.

In progress: bulk data between tasks ([decision 0032](docs/decisions/0032-bulk-data-copied-through-the-callers-windows.md):
a kernel copy through windows the caller gives, in place of Mach's
out-of-line memory), already in the model, proved and in the kernel's
calls; MIG's stubs and the measurements are next. Then the phase's
milestone: libtrivfs and the Hurd's `hello` translator, the first Hurd
server running unmodified on Joist.

**Ahead.** The paging protocol and libpager (Phase 6), the rest of the
glibc port (7), and bootstrapping the Hurd to a `/bin/sh` prompt running
coreutils (8), the next large visible goal; then resilience (servers
restarted transparently, the Minix 3 goal), drivers, server hardening and
POSIX conformance. The foundation took less time than the plan's estimates;
the phases ahead hold most of the problems the plan calls hard (paging,
the boot chain, drivers).

**Known open items:** Joist's own locks inside glibc must become critical
sections before a thread can be aborted mid-call (Phase 7); robust mutexes
can't yet notice a thread that died holding one; and whether a copy is fast
enough for `io_read`, against a page move, is to be measured before Phase 11.

Design decisions, each with its options and the reason for the choice, are
in [`docs/decisions/`](docs/decisions); the native ABI is
[`docs/abi.md`](docs/abi.md).

## Layout

| Path | What |
|---|---|
| `kernel/` | the kernel: `core/` (the object layer, also run on the host as the model), `sys/` (the system-call ABI), `arch/x86_64/`; its tests and fuzzers |
| `model/` | the host model's test runner: property tests under every sanitizer, mutations, libFuzzer, CBMC, TLA+ |
| `servers/mm/` | the memory manager (decision 0023) |
| `user/` | test programs and the root tasks the self-tests boot |
| `glibc/` | Joist's files for glibc (`sysdeps/joist`) and their host tests |
| `third_party/mig/` | GNU MIG, unchanged, plus Joist's output mode (`joistmig.c`) |
| `tools/` | measurement scripts, the glibc cross-build and its patches, the MIG tests, reproducibility checks |
| `mcp-server/` | a small MCP server for querying the plan |
| `docs/` | plans, exit records, decisions, data |

## Building and testing

Clang/LLVM, lld, QEMU and xorriso; Limine for booting.

```sh
make -C model            # the host model's tests
make -C model check      # ... under ASan, UBSan, MSan, TSan and tiny tables
make -C kernel test      # the kernel's host tests
make -C kernel kernel    # the kernel image
sh kernel/test/selftest.sh   # boot every self-test under QEMU (and KVM where present)
make -C glibc/test check # sysdeps/joist's host tests
```

glibc's cross-build for Joist is `tools/phase5/glibc-cross.sh WORKDIR`.

## Licence

Joist is **GPL-3.0-only** ([`LICENSE`](LICENSE)), with these exceptions:

- **`glibc/sysdeps/`** is **LGPL-2.1-or-later**, as the rest of glibc, so
  programs stay linkable as with any glibc and the code can go upstream.
- **Code generated by `joistmig`** is under the licence of the `.defs` it
  was generated from, not joistmig's (the exception is stated in
  `third_party/mig/joistmig.c`).
- **Third-party code keeps its own licence:** GNU MIG in
  `third_party/mig/` (Carnegie Mellon's Mach licence and GPL-2.0-or-later;
  see `third_party/mig/JOIST.md`), Limine's boot protocol header
  `kernel/include/limine/limine.h` (0BSD), cJSON in `mcp-server/vendor/`
  (MIT).
