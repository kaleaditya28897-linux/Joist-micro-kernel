# Copilot instructions for Joist

Joist is a from-scratch C microkernel intended to replace GNU Mach beneath the GNU Hurd,
aiming for QNX-level reliability and Minix 3-level resilience while staying POSIX compatible.

## Repository state (read this first)

**Phases 0, 1 and 2 are closed** (exit records `docs/phase0-exit.md`, `docs/phase1-exit.md`,
`docs/phase2-exit.md`, accepted 2026-10-01, 2026-10-04 and 2026-10-05). **Phase 3 (IPC and
capabilities in the real kernel) is next**; its inputs are the *Carried forward* lists in
those records. What exists:

- `joist-microkernel-plan.md`: the authoritative design document.
- `docs/decisions/`: the design journal, **0000–0015 all accepted** (index in its README;
  0001 and 0004 carry accepted amendments, 0015 chose Limine).
- Phase 0 evidence: `docs/mach-inventory.md`, `docs/clang-inventory.md`,
  `docs/client-recovery.md`, `docs/rpc-idempotency.md`, `docs/phase0-data/`.
- Phase 1: `docs/phase1-plan.md`, `docs/phase1-data/`, `docs/abi.md` (the native API as the
  model settled it; Phase 3 fixes the binary ABI), and `model/`, the host-side semantic
  model (invariants I1–I10, property tests, mutation, libFuzzer, TLA+ in `model/tla/`, CBMC
  in `model/cbmc/`). See `model/README.md`.
- Phase 2: `docs/phase2-plan.md`, `docs/phase2-data/`, and `kernel/`, the freestanding
  x86_64 kernel booted by Limine (`boot/limine.conf`), with `user/`'s ring-3 test programs
  as root task. See `kernel/README.md`, `user/README.md`. `make check` in `kernel/` runs
  host tests under every sanitizer, `clang-tidy`, `scan-build`, BIOS and UEFI boots, and the
  self-tests.
- `tools/phase0/`, `tools/phase1/`: the scripts and patches behind each phase's data.
- `docs/testbed.md`: the Debian GNU/Hurd x86_64 VM used for measurements.

`joist-microkernel-plan.md` is the source of truth. Keep it internally consistent: section
numbers (§N), the phase roadmap (§11), the decision list (0000–0015), and the cross-references
between them are load-bearing. If you change a design commitment, update every place it is
referenced, not just the local paragraph.

## The non-negotiables the plan already commits to

These are decided, not open questions. Do not silently reopen them:

- **Priority order when goals conflict (§1.1):** Reliability > Resilience > POSIX > Reuse of
  Hurd > Performance. Every design decision must be justifiable against this order; the design
  journal template (§15) requires an explicit "priority check".
- **Mach-shaped, not Mach-implemented (§4.2):** the kernel exposes a small native
  capability/IPC API; every Mach concept the Hurd uses is mapped onto it in *userspace*
  (retargeted MIG stubs, adapted libports, glibc `sysdeps/joist`), never in the kernel.
- **Kernel discipline (§5.1):** no dynamic allocation after boot, no recursion, no VLAs,
  bounded worst-case execution time for every syscall, user pointers only touched through
  `copy_from_user`/`copy_to_user`, kernel core targeted under ~15k lines (a review tripwire,
  not a hard build failure).
- **Reuse loses every tie against reliability/resilience.** Reusing Hurd code is a
  time-saving means, not a goal; rewrite in clean C wherever reuse would break the top goals.

## Conventions specific to this project

- **Decision journal.** Non-trivial design decisions become `docs/decisions/NNNN-title.md`
  using the template in §15 (Context / Options / Decision / Consequences / Priority check).
  0000–0015 are **accepted**. A change of mind is a new, superseding entry, never an edit;
  amendments record new evidence or settle a decision's listed open items. 0001's five open
  parameters were settled by its 2026-10-03 amendment: 4 × 64-bit message registers plus a
  4 KiB IPC buffer, 64-bit server-chosen badges, a 64-bit notification word, a two-level
  capability table, and no bare `Send` (deferred replies use reply objects).
- **Measurements are auditable.** Data goes in `docs/phase0-data/` (or a later phase's
  equivalent) as CSV/TSV with the producing command, tool versions and raw-log checksums
  recorded in that directory's README. Numbers in documents are generated from or traceable
  to those files.
- **Every server has a class (§7.2).** A (stateless/idempotent), B (recoverable via DS),
  or C (critical core). "No server ships without a class" — treat an unclassed server as
  incomplete.
- **Every reused RPC is classified idempotent / non-idempotent (§7.3.4),** checked in as a
  table. Unclassified is a build error, not a runtime guess. For a non-idempotent call whose
  reply was lost, fail with `EIO` rather than replay.
- **Client-side recovery lives in one glibc/Hurd sysdeps module (§7.3),** keyed on the
  *file description* (not the descriptor), with identity validated by inode+generation, not
  path alone.
- **Symbol/name prefix is `joist_`** (libraries `libjoist*`); the old working name "Sherpa"
  is dropped (§17, decision 0000). Do not reintroduce it.

## Intended architecture (the big picture)

- **Kernel (`kernel/`, freestanding C):** tasks, threads, endpoints, badged capabilities,
  notification objects, memory frames, IRQ/IO/DMA capabilities, scheduling contexts. Provides
  synchronous send/receive/reply IPC, capability lifetime notifications (no-senders /
  dead-name / send-once — the things Hurd relies on Mach for), and page-fault-as-IPC. It does
  *not* contain filesystems, drivers (beyond timer/interrupt basics), POSIX, signals, or
  paging policy (§5.7).
- **Servers (`servers/`, new C):** reincarnation server (RS) supervises/restarts, data store
  (DS) holds opaque per-publisher recovery blobs, memory manager (mm) implements mmap/COW/
  anonymous+file-backed memory over the page-fault protocol.
- **Reused userland:** retargeted MIG (keep `.defs`, new native backend), adapted libports,
  rewritten libpager, mostly-kept libdiskfs/libnetfs/ext2fs/etc., and a glibc port that swaps
  `sysdeps/mach` for `sysdeps/joist` while keeping `sysdeps/mach/hurd` edits minimal.

The proposed tree and phase-by-phase roadmap (each phase has a hard exit criterion) are in
§11–§12 (§12 carries a note on how the real tree differs); follow the phase ordering and exit criteria when adding real code.

## Tooling the plan mandates

- **Compiler:** Clang/LLVM pinned to one major version for everything Joist owns; `ld.lld`,
  `llvm-objcopy`/`objdump`/`ar`. Decision 0002 is accepted on the **Clang branch**: glibc too
  is built with Clang, on a Linux host cross-compiling for the Hurd (clang running *on* the
  Hurd hangs on glibc's math headers; see 0002). Every Makefile guards on `$(origin CC)`,
  because `CC ?= clang` never fires.
- **Build system:** plain Makefiles. **No CMake or meta-build layers** (KISS).
- **Kernel flags (x86_64):** `--target=x86_64-unknown-none-elf -std=c17 -ffreestanding
  -nostdlib -fno-builtin -fno-stack-protector -fno-pic -mcmodel=kernel -mno-red-zone
  -mgeneral-regs-only -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow`.
- **Verification (§8.1), warnings-as-errors, from day one:** kernel data structures compile
  both freestanding and on the host for unit tests; host tests run under ASan/UBSan/MSan/TSan;
  in-kernel trap-mode UBSan in debug builds; `clang-tidy` (CERT + bugprone) and `scan-build`;
  libFuzzer + syzkaller-style syscall fuzzing; CBMC for capability/IPC invariants.
- **Formatting/linting:** `/.clang-format` is checked in (4-space indent, Linux braces,
  braces everywhere, 100 columns; applied to new code, not retrofitted to `mcp-server/`).
  `/.clang-tidy` is checked in (CERT, bugprone, analyzer) and runs in `kernel/`'s `make check`.
- **Kernel debugging:** QEMU + GDB (`-s -S`), serial log as primary output. Installed:
  clang/LLD 21.1.8, QEMU 11.1.1, Limine 12.9.2, xorriso, GDB, clang-tidy, scan-build (system);
  TLA+ tools v1.7.4 and CBMC 6.11.0 (user-local, `~/.local`).

When you introduce any of these, add the corresponding config/Makefile rather than running
ad-hoc commands, so future sessions inherit a reproducible setup.

## Tooling already in-tree

- **`mcp-server/`** — a small C (Clang/GCC, plain Makefile) Model Context Protocol server
  that exposes the design plan as query tools (`list_sections`, `get_section`, `get_phase`,
  `list_decisions`). Build with `make` (or `make CC=gcc`), test with `make test`, register via
  the CLI's `/mcp` command. See `mcp-server/README.md`. It follows the same strict warning set
  and KISS Makefile policy expected of Joist's own code.
