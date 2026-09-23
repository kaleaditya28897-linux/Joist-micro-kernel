# Copilot instructions for Joist

Joist is a from-scratch C microkernel intended to replace GNU Mach beneath the GNU Hurd,
aiming for QNX-level reliability and Minix 3-level resilience while staying POSIX compatible.

## Repository state (read this first)

This repo is **pre-implementation**. The only content today is `joist-microkernel-plan.md`,
a single authoritative design document. There is no kernel source, build system, tests, or
tooling in-tree yet. When asked to "build", "test", or "run" something, there is nothing to
run — the correct action is usually to edit the plan or scaffold new work consistent with it.

`joist-microkernel-plan.md` is the source of truth. Keep it internally consistent: section
numbers (§N), the phase roadmap (§11), the decision list (0000–0003), and the cross-references
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
  0000 (name) is settled; 0001 (IPC model), 0002 (Clang-vs-GCC for glibc), 0003 (SMP +
  no-runtime-alloc + bounded WCET) are the next required entries.
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
§11–§12; follow the phase ordering and exit criteria when adding real code.

## Tooling the plan mandates (when code lands)

- **Compiler:** Clang/LLVM pinned to one major version for everything Joist owns; `ld.lld`,
  `llvm-objcopy`/`objdump`/`ar`. glibc may fall back to **GCC** under the pre-committed policy
  in §13 (decision 0002) — that is the *only* sanctioned second compiler.
- **Build system:** plain Makefiles. **No CMake or meta-build layers** (KISS).
- **Kernel flags (x86_64):** `--target=x86_64-unknown-none-elf -std=c17 -ffreestanding
  -nostdlib -fno-builtin -fno-stack-protector -fno-pic -mcmodel=kernel -mno-red-zone
  -mgeneral-regs-only -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow`.
- **Verification (§8.1), warnings-as-errors, from day one:** kernel data structures compile
  both freestanding and on the host for unit tests; host tests run under ASan/UBSan/MSan/TSan;
  in-kernel trap-mode UBSan in debug builds; `clang-tidy` (CERT + bugprone) and `scan-build`;
  libFuzzer + syzkaller-style syscall fuzzing; CBMC for capability/IPC invariants.
- **Formatting/linting:** checked-in `clang-format` and `clang-tidy` configs.
- **Kernel debugging:** QEMU + GDB (`-s -S`), serial log as primary output.

When you introduce any of these, add the corresponding config/Makefile rather than running
ad-hoc commands, so future sessions inherit a reproducible setup.

## Tooling already in-tree

- **`mcp-server/`** — a small C (Clang/GCC, plain Makefile) Model Context Protocol server
  that exposes the design plan as query tools (`list_sections`, `get_section`, `get_phase`,
  `list_decisions`). Build with `make` (or `make CC=gcc`), test with `make test`, register via
  the CLI's `/mcp` command. See `mcp-server/README.md`. It follows the same strict warning set
  and KISS Makefile policy expected of Joist's own code.
