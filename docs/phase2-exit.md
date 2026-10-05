# Phase 2 exit

**Status: proposed, 2026-10-05.** Phase 2 (§11, *Kernel bootstrap*) closes
when the project owner accepts this record. The work, step by step, is in
`docs/phase2-plan.md`, *Progress*; data in `docs/phase2-data/`.

## Exit criterion

| Criterion (§11) | Met by |
|---|---|
| A userspace "hello" task runs | `user/hello`, loaded as the root task from a Limine module through the ELF validator, runs in ring 3 under BIOS and UEFI, prints through `debug_write` and exits 0 (`make qemu-test`, both firmwares, in `make check`) |
| The kernel survives fuzzing of its tiny syscall surface | `user/sysfuzz`: random call numbers, pointers from every interesting class, random lengths, every other register random; each call must return with registers preserved and the predicted result. 1 000 000 calls in every `make check` (also under the UBSan kernel), 30 000 000 more over three seeds (`make fuzz-kernel`). Host-side, libFuzzer on the dispatcher (36 million inputs) and the ELF validator (47 million) |

## Activities

| Activity (§11, `phase2-plan.md`) | Status |
|---|---|
| Freestanding C kernel for x86_64, Clang, `-ffreestanding -nostdlib` | done, with §13's flags, linked with `ld.lld` |
| Boot (Limine, decision 0015) | done, BIOS and UEFI, base revision 6 |
| Serial console | done (COM1, polled) |
| GDT/IDT | done, plus a TSS (kernel stack, IST1 for double faults, no I/O bitmap) |
| Paging | done: the kernel's own tables, W^X, NX, a direct map of RAM only |
| Physical frame allocator | done, boot-time, two bitmaps (taken / never allocatable) |
| APIC timer | done, PIT-calibrated within 0.1% of QEMU's clock, 100 Hz |
| Load a root task from a boot module, run it in user mode | done |
| Minimal debug syscalls | done: `debug_write`, `exit` |
| §8.1: host tests and sanitizers on kernel data structures | done: 25 host tests under plain, ASan, UBSan, MSan, TSan; libFuzzer on the ELF validator and the dispatcher; 30 mutations, all caught |
| §8.1: UBSan in the kernel | done: a trap-mode debug build, checks named |
| §8.1: `clang-tidy`, `scan-build`, warnings as errors | done, both in `make check` and both shown to be live |

## Found along the way

- **A register restore missed by mistake would have been invisible.** The
  fuzzer didn't notice a planted unrestored `r10` because the C handler
  happened not to touch it. The `syscall` stub now zeroes every
  caller-saved register before restoring the user's, so a missed restore
  hands back 0, never a kernel value, and is detected.
- **The `sysret` hazard**, refused in C and tested at runtime by placing
  `syscall` in the last two bytes of the user half. QEMU raises the
  resulting fault AMD-style (in ring 3), so it can't show the dangerous
  Intel form, but it does show guard versus no guard.
- **Tests that passed by coincidence**, found by mutation: a large-page check
  that hit another present large page, a wrapping ELF range that failed for
  another reason, a header-overrun test whose image sat in a large static
  buffer. Each now has a test that isolates it.

## Carried forward

- **To Phase 3:** the real system-call ABI (`docs/abi.md`) replaces the two
  debug calls; capabilities, IPC and more than one task; fault IPC (0009)
  replaces "the root task faulted, the run ends"; SMP (0003); per-task
  address spaces need the kernel half kept in sync (today copied once, with
  no kernel mappings made after the root task starts). And the three items
  still open in `docs/abi.md`: capability transfer, preemptible cascades
  (with TLA+'s rule that a dying space refuses capabilities), revoke and
  the batched copy.
- **Phase 2 shortcuts, to revisit:** the frame allocator and the
  `debug_write` buffer are boot-time and single-CPU; the timer only counts;
  `-fstack-protector-strong` (§13, "once the basics work") is not on yet.
- **Real hardware** (§5.8 target 2): untested; everything here ran in QEMU.

## The kernel at exit

| | |
|---|---|
| Kernel C | 2 063 lines (1 166 x86_64-specific, 897 portable core), plus 185 lines of assembly; the plan's Phase 2 expectation was under 3 000, the overall tripwire 15 000 (§5.1) |
| Tests | 25 host tests, 16 boot self-tests, 4 user self-test tasks plus the fuzzer, 30 mutations |
| Toolchain | clang/LLD 21.1.8, QEMU 11.1.1, Limine 12.9.2 |
