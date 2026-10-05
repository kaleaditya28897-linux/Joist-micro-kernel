# 0015 — Boot protocol: Limine

Date: 2026-10-05
Status: accepted (2026-10-05)
Covers: §5.8 ("Boot via Multiboot2 (GRUB) or Limine"), Phase 2's first step.
Blocks: Phase 2 (kernel bootstrap).

## Context

§5.8 leaves the boot protocol open between two: Multiboot2 (loaded by GRUB)
and Limine. The kernel needs from its bootloader: to start on x86_64, a
memory map, the boot modules (the root task of 0013, later the bootstrap
servers), a framebuffer or at least a serial console, and the ACPI tables
(the APIC timer needs the MADT). §5.1 asks that assembly and raw page-table
work stay in a few audited modules, and the less of it the better.

## Options considered

**A — Multiboot2, loaded by GRUB.** The standard on the Hurd today: GNU Mach
boots this way, so Debian's GRUB setup already knows it. But Multiboot2 hands
over in **32-bit protected mode with paging off**. The kernel must ship its
own trampoline: build a first set of page tables, enable PAE and long mode,
and jump to 64-bit code, all in assembly before any C runs. That is a well
known 150–300 lines, but it is exactly the code §5.1 wants minimised, and it
runs before the serial console, so its bugs are the hardest to debug.

**B — Limine (its native boot protocol).** Hands over **already in 64-bit
long mode**, with the kernel loaded at its higher-half link address, a
higher-half direct map of physical memory, and a 16-byte-aligned stack. The
kernel asks for what it needs through C structures it places in a dedicated
section (memory map, modules, RSDP, framebuffer, SMP start-up), so the entry
point can be C almost at once. Boots from BIOS and UEFI, in QEMU and on real
hardware. Maintained (v12.9, October 2026) and packaged by Void (12.9.1).
Its protocol header is 0BSD-licensed and is meant to be copied into the
kernel's tree. The cost: one more bootloader in the Hurd world, and the
kernel depends on Limine's protocol (Limine also speaks Multiboot2, so
moving back later is a matter of an entry stub, not a redesign).

**C — Both from the start.** Two entry paths to test from day one, for a
benefit (GRUB compatibility) nobody needs until Phase 8 boots a real Hurd.

## Decision

**B: Limine**, native protocol, for Phase 2 onwards. Multiboot2 is
reconsidered, as an extra entry stub, only if a target can't run Limine.

## Consequences

- Easier: the assembly in Phase 2 shrinks to the interrupt and system-call
  entry stubs and the context switch; there is no 32-to-64-bit trampoline.
  Memory map, modules and ACPI arrive as C structures, which a host test can
  build fakes of, so the boot-information parsing is host-testable (§8.1).
- Easier: SMP start-up (0003) comes from Limine's MP request rather than a
  hand-written INIT-SIPI trampoline, when Phase 3 needs it.
- Harder: Debian GNU/Hurd boots with GRUB; Phase 8 has to put Limine on the
  disk image or add a Multiboot2 stub then. The kernel trusts Limine's
  page tables until it installs its own, which Phase 2 does early.

## Priority check

Respects Reliability > Resilience > POSIX > Reuse > Performance. Reliability
decides it: B removes the least testable code in the kernel. Reuse (GRUB,
which the Hurd already uses) loses, as the order requires, and the loss is
recoverable later.
