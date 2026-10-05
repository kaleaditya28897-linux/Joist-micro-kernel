# Phase 2 plan: kernel bootstrap

Plan §11, Phase 2: *a freestanding C kernel for x86_64 built with Clang
(`-ffreestanding -nostdlib`): boot (Multiboot2/Limine), serial console,
GDT/IDT, paging, physical frame allocator, APIC timer. Load a root task from a
boot module and run it in user mode. Minimal debug syscalls (print to
serial).* **Exit: a userspace "hello" task runs; the kernel survives fuzzing
of its tiny syscall surface.**

This file turns that into concrete work, as `docs/phase1-plan.md` did for
Phase 1. It is a plan, not a decision: where it chooses something the
decisions don't, the choice is marked and can be changed without a journal
entry until later phases build on it. The one choice big enough for the
journal, the boot protocol, is proposed as decision 0015.

## Scope

| In Phase 2 | Not until later |
|---|---|
| Boot on x86_64 in QEMU (BIOS and UEFI), serial console | real hardware (§5.8 target 2) |
| GDT, TSS, IDT; every exception reported, never silently lost | fault IPC to a handler (0009, Phase 3) |
| The kernel's own page tables (higher-half kernel, user low half) | `vm_*` and the memory manager (0008, Phase 4) |
| A physical frame allocator, used at boot | memory donated by tasks, untyped objects (0003, 0004; Phase 3–4) |
| Local APIC timer, periodic tick | scheduling contexts, more than one runnable task (§5.6, Phase 3) |
| One root task, loaded from a boot module (ELF64), in ring 3 | capabilities, IPC, more tasks (Phase 3: the model's ABI) |
| Debug system calls: write to serial, exit | the real system-call ABI (0001 amendment, Phase 3) |
| Fuzzing the system-call surface | — |

One CPU. SMP (0003) starts in Phase 3, once there is something to share.

## Constraints carried in

- **§5.1:** no recursion, no VLAs, fixed-width integers, every return value
  checked, every `switch` with a `default`; assembly and page-table code only
  in `kernel/arch/` and `kernel/mem/`; **user pointers never dereferenced**,
  only through `copy_from_user`/`copy_to_user` with bounds and fault handling;
  the ~15k-line tripwire counted from the start.
- **§5.1 allocation rule:** the frame allocator is a *boot-time* allocator. No
  kernel heap. Everything the kernel allocates after boot will be paid for by
  a task (Phase 3–4); in Phase 2 nothing is allocated after the root task
  starts.
- **§13 flags:** `--target=x86_64-unknown-none-elf -std=c17 -ffreestanding
  -nostdlib -fno-builtin -fno-stack-protector -fno-pic -mcmodel=kernel
  -mno-red-zone -mgeneral-regs-only -Wall -Wextra -Wpedantic -Werror
  -Wconversion -Wshadow`, linked with `ld.lld`; plain Makefiles; clang 21 as
  pinned (0002).
- **§8.1:** data structures compile both freestanding and on the host, and are
  tested there with the sanitizers; UBSan in trap mode in the kernel's debug
  build; `clang-tidy` (CERT, bugprone) and `scan-build` with warnings as errors;
  fuzzing of the system-call surface.

## Choices this plan makes

1. **Boot: Limine's native protocol** (decision 0015, proposed). The kernel
   starts in long mode, higher half, with a direct map of physical memory;
   requests for the memory map, modules, RSDP and framebuffer are C structures.
   Limine's 0BSD protocol header is copied into the tree.
2. **Memory layout** (changeable). Kernel linked at `0xffffffff80000000`
   (`-mcmodel=kernel`); a direct map of physical memory in the upper half (at
   Limine's offset at first, then the kernel's own); user space in the lower
   half. The kernel builds and loads its own page tables early in boot and
   stops relying on Limine's: writable xor executable, user/supervisor bits
   set deliberately, NX on everything not code.
3. **Frame allocator** (changeable): one bit per 4 KiB frame over the usable
   ranges of the memory map, the bitmap itself placed in a usable range. Simple,
   bounded per allocation by the bitmap size, trivially host-testable. It is
   boot-time only; Phase 4's donated-memory scheme replaces its callers.
4. **Interrupts.** All 256 IDT vectors have stubs; an exception in the kernel
   prints the full register state and halts; one in user mode prints and ends
   the task (Phase 3 turns that into fault IPC). The legacy PIC is masked.
5. **Timer:** the Local APIC timer, periodic, calibrated once against the PIT
   (both exist in QEMU and on the target hardware). Phase 2 only counts ticks:
   there is nothing to schedule.
6. **System calls:** `syscall`/`sysret` (STAR, LSTAR, SFMASK), with the
   `sysret` non-canonical-RIP hazard handled. Two debug calls:
   `debug_write(ptr, len)`, which copies from user memory through the checked
   path and writes to serial, and `exit(code)`. Anything else returns an
   error. The register layout follows the 0001 amendment's shape (number in
   `rax`, arguments in `rdi`, `rsi`, `rdx`, `r10`, `r8`), so Phase 3 extends it
   rather than replacing it.
7. **The root task:** a static ELF64 executable loaded from a Limine module,
   validated before anything is mapped (every header field range-checked, no
   overlapping or kernel-half segments), one thread, entered with `sysret` or
   `iretq`. Phase 2's root task is `hello`; 0013's real root task comes in
   Phase 3.
8. **Repository layout** (changeable), following §12: `kernel/` with
   `arch/x86_64/` and `mem/`; `user/hello/` and `user/sysfuzz/` for the two
   user programs; `boot/` for the Limine configuration; a top-level Makefile
   building an ISO.

## Testing

| What | How |
|---|---|
| Frame allocator, ELF validator, boot-information parsing, `copy_*_user` bounds logic, system-call argument decoding | compiled for the host too, unit- and property-tested under every sanitizer, as in `model/`; libFuzzer on the ELF validator and the system-call decoder |
| Boot | `make qemu-test`: boot the ISO headless (BIOS and UEFI), serial to a file, check the expected lines, end through QEMU's `isa-debug-exit` device so the exit code is the result |
| The exit criterion, "hello" | the same, with the root task printing through `debug_write` |
| The exit criterion, "survives fuzzing" | `user/sysfuzz`: a root task that issues random system calls with random registers, including kernel-half and unmapped pointers, from a seed, for N calls. The kernel must not panic, and every call must return. Plus host libFuzzer on the decoder. Seeds that crash are kept as regression tests |
| Kernel UBSan | debug build with `-fsanitize=undefined -fsanitize-trap=undefined`, and a trap handler that names the check |
| Static analysis | `clang-tidy` (CERT, bugprone) and `scan-build` over `kernel/`, warnings as errors, in `make check` |
| Mutation | as Phase 1: injected bugs in the host-testable modules must be caught |

## Order of work

1. **Build and boot skeleton:** Makefile, linker script, Limine ISO, serial
   output from a C entry point, `make qemu-test`. (Decision 0015 first.)
2. **GDT, TSS, IDT**, exception reporting; a deliberate fault test.
3. **Frame allocator**, host-tested first.
4. **Kernel page tables**: switch off Limine's tables onto the kernel's own.
5. **Local APIC timer**, calibrated; tick count on serial.
6. **User mode**: ELF validator (host-tested), root task loading, `syscall`
   entry, `debug_write` and `exit` with `copy_from_user`. "hello" runs:
   **first half of the exit criterion.**
7. **System-call fuzzing**: `user/sysfuzz` in QEMU and host libFuzzer:
   **second half.**
8. **Static analysis and the debug UBSan build** in `make check`; exit record.

## Tooling

Needed, not installed (root): `ld.lld`, `llvm-objcopy`/`llvm-objdump`/
`llvm-ar` (§13), QEMU for x86_64 with its UEFI firmware, `xorriso` (ISO),
Limine, GDB, and the static analysers:

```sh
sudo xbps-install -S lld llvm qemu-system-amd64 qemu-firmware xorriso limine gdb \
    clang-tools-extra clang-analyzer
```

Void's `limine` is 12.9.1 and `qemu-system-amd64` 11.1.1.

## Risks

- **`sysret` and `swapgs` corner cases** are classic kernel holes (the
  non-canonical return address, entering with a user GS). They are the first
  targets of the system-call fuzzer and get a written argument in the code.
- **Calibration of the APIC timer** varies between QEMU and hardware; Phase 2
  only needs it to tick, not to be accurate.
- **The tripwire:** the 15k-line count starts now; Phase 2 should stay well
  under 3k lines of C plus a few hundred of assembly.

## Progress

### Step 3: the frame allocator, host side (2026-10-05)

Done ahead of steps 1–2, which need the toolchain (`ld.lld`, QEMU, Limine)
and decision 0015. `kernel/mem/frame.[ch]`, built and tested from `kernel/`
(`make check`).

- **Two bitmaps**, one bit per frame each: `taken` (not available now) and
  `fixed` (never available: gaps between usable ranges, reservations such as
  the kernel image, the tail past the last frame). With one bitmap, freeing a
  frame that was never handed out (the kernel image, a gap) would have been
  indistinguishable from freeing an allocated one; now it is refused
  (`KERR_STATE`), as is a double free.
- **The bootloader's memory map is checked, not trusted**: unsorted,
  overlapping, empty or address-wrapping ranges make `frame_init` refuse the
  whole map; ranges are trimmed inwards to whole frames.
- **Bounded** by the bitmaps' size, fixed at boot by the machine's memory.
  No allocation: the storage is the caller's.
- **Compiles freestanding** for `x86_64-unknown-none-elf` with §13's flags
  (`make freestanding`; objects only until `ld.lld` is installed).

| Check | Result |
|---|---|
| Unit tests (`kernel/test/test_frame.c`) | 6: malformed maps (unsorted, overlapping, empty, wrapping after a valid range, no whole frame, bad storage), inward trimming, gaps never handed out or freed, `frame_put` errors, permanent reservations, a 64 MiB map allocated to exhaustion |
| Property test | 500 seeds × 2 000 steps of gets and puts (including misaligned, out-of-range, double and gap frees) against a shadow of every frame's state; every result reached: about 440 000 gets, 58 000 out-of-memory, 390 000 puts, 64 000 / 33 000 / 14 000 refused as misaligned / out of range / not taken |
| `make check` | plain, ASan, UBSan, MSan, TSan; freestanding compile |
| `make mutate` (`kernel/test/mutations.txt`) | 10 of 10 caught. The first run found a test gap (a wrapping range alone was refused for another reason, so the wrap check was never tested) and a mutation that wasn't a bug (picking a different free frame); both fixed |

### Step 6, host side: the ELF validator (2026-10-05)

`kernel/elf/elf.[ch]`. The root task arrives as a boot module, untrusted
bytes; `elf_validate` checks all of it before anything is mapped, and hands
the loader a fixed-size description. Fields are read as little-endian bytes
(no casts, so alignment is irrelevant), nothing outside the file is read,
and the work is bounded by `ELF_MAX_PHDRS` (16).

Accepted: a static x86_64 executable whose loadable segments lie in the user
range, sorted and not sharing a page, offset and address congruent modulo
4 KiB, file size no larger than memory size, never both writable and
executable (W^X), with the entry point in an executable segment. Refused:
`PT_DYNAMIC`, `PT_INTERP`, `PT_TLS`, and anything malformed or overflowing.

| Check | Result |
|---|---|
| Unit tests (`kernel/test/test_elf.c`) | 5: a valid two-segment executable; every header rule; every segment rule (overflowing offsets and addresses, out of range, unsorted, sharing a page, W^X, empty); entry point and refused types; program headers running past the end (in an exact-sized heap buffer, so ASan sees any over-read) |
| Property test | 200 000 corrupted images (bit flips, nasty field values, truncation): about 27 000 accepted, 173 000 refused, and every accepted one checked against everything `elf.h` promises by an independent checker (`test/elf_promises.h`) |
| libFuzzer (`make fuzz-elf`) | the same promises on arbitrary input under ASan and UBSan, from a seed corpus of one valid image (`kernel/fuzz/elf-seeds/`): 47 million inputs in 2 minutes, no crash. With W^X removed, it finds the bug in under 800 000 inputs |
| `make mutate` | 18 of 18 caught (10 allocator, 8 validator). One validator mutation first survived: a test meant to catch headers running past the file failed for another reason; a dedicated test now isolates it |
| `make check` | 13 tests under plain, ASan, UBSan, MSan, TSan; both kernel sources compile freestanding |
