# kernel/: the Joist kernel

The freestanding C kernel for x86_64, booted by Limine (decision 0015).
Scope and order of work: `docs/phase2-plan.md`; what was delivered and
what is carried forward: `docs/phase2-exit.md`.

**State:** Phase 2 (*Kernel bootstrap*) is closed (accepted 2026-10-05);
Phase 3 (*IPC and capabilities*) is under way (`docs/phase3-plan.md`). The
kernel boots under BIOS and UEFI and sets up a GDT, TSS (IST1 for double
faults) and IDT, a boot-time frame allocator, its own page tables (W^X, NX)
and a 100 Hz Local APIC timer. Since Phase 3 step 4 it runs the object
layer in `core/`: many tasks, each with its own address space, threads
switched round robin, and the system-call ABI of `docs/abi.md`
(`sys/abi.c`). The root task is loaded from the first Limine module and
given the memory and the other modules as frame capabilities; it starts
the rest.

## Layout

| Directory | What |
|---|---|
| `arch/x86_64/` | Boot entry (`boot.c`), GDT/TSS, IDT and exception stubs (`isr.S`), the `syscall` entry (`syscall.S`), serial, APIC, the kernel's paging setup, `run.c` (address spaces, threads, in and out of user mode, the root task), `linker.ld`. All assembly lives here |
| `mem/` | The frame allocator (`frame.c`) and page tables (`vm.c`); host-testable |
| `elf/` | The ELF validator for boot modules; host-testable |
| `sys/` | The system-call ABI (`abi.c`: registers to core operations), Phase 2's debug calls (`syscall.c`) and `copy_from_user` (a software page walk); host-testable |
| `core/` | The object layer: capabilities, endpoints, IPC, notifications, lifetime events, tasks and threads (API: `include/joist/core.h`). The Phase 1 model, moved here in Phase 3; its tests, fuzzer, mutations, CBMC and TLA+ are in `../model` |
| `lib/` | `kprint` and the string functions the freestanding build needs |
| `include/` | `joist/` (kernel error codes, `kprint`) and Limine's 0BSD protocol header |
| `test/` | Host unit and property tests, the mutation list, the boot self-test script |
| `fuzz/` | libFuzzer harnesses for the ELF validator and the dispatcher |

The sources in `mem/`, `elf/`, `sys/` and `core/` compile both freestanding
and on the host, so they are tested under every sanitizer (§8.1); `core/` by
`make check` in `../model`.

## Build and test

Needs clang/LLD 21, QEMU, Limine, xorriso (versions in
`docs/phase2-exit.md`). The root task is built from `../user`.

```sh
make              # host tests (plain)
make check        # host tests under plain, ASan, UBSan, MSan, TSan; freestanding
                  # compile; clang-tidy; scan-build; boot under BIOS and UEFI; self-tests
make kernel       # build/joist.elf
make iso          # build/joist.iso (BIOS and UEFI)
make qemu-test    # boot headless, check the serial log (FIRMWARE=bios|uefi)
make selftest     # builds that fault on purpose; each must be reported correctly
make fuzz-kernel  # the system-call fuzzer as root task (SEEDS, CALLS)
make mutate       # each bug in test/mutations.txt must fail the host tests
make fuzz-elf     # libFuzzer on the ELF validator (FUZZ_TIME seconds)
make fuzz-syscall # libFuzzer on the dispatcher
make tidy | scan | format | clean
```

`ROOT_TASK=<name>` picks which `../user` program boots as the root task
(default `hello`), and `MODULES="a b"` adds further boot modules for it to
load: `make BUILD=build/pp ROOT_TASK=root MODULES="pong ping" build/pp/joist.iso`
is Phase 3's ping-pong (self-test `pingpong`). `make fuzz-abi` fuzzes the
whole system-call ABI on the host.
