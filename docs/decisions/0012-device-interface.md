# 0012 — Device interface → IRQ / IO port / MMIO / DMA capabilities

Date: 2026-09-23
Status: accepted (2026-10-01)
Covers: §6 row *Device interface (`device_open`, etc.)*.

## Context

Mach's device interface (`device_open`, `device_read`, `device_write`,
`device_map`) exposes kernel-resident drivers to userspace. §6 maps it onto
IRQ/IO/MMIO/DMA capabilities handed to userspace drivers, in the rumpdisk and
netdde glue. §4.1 keeps rumpdisk and netdde as-is; §2 notes modern Hurd has
already moved storage and networking out of the kernel.

**Evidence.** Across all three workloads, **no `device_*` call fires** except
`device_pager_init_pager` (1, at boot). The build workload never touches the
device interface at all.

Two readings, and both are partly true. First, the trace runs on a VM whose
storage and network are already handled by userspace drivers, so the legacy
in-kernel device path genuinely is not used for I/O. Second, the coverage limit
applies: boot was traced via an ext2fs translator proxy, not from the real
first instruction, so early device setup is under-observed
(`docs/mach-inventory.md` *Coverage*).

The conclusion the data supports is modest but useful: **the device interface
is not on any hot path**, so it can be replaced wholesale rather than
carefully emulated.

## Decision

- **There is no device interface in Joist.** `device_open` and friends have no
  counterpart. A driver is an ordinary userspace server that holds capabilities
  for the hardware it owns:
  - an **IRQ capability**, which binds one interrupt to a notification object
    (§5.2, 0001) — interrupts arrive as notification bits, never as messages,
    so an interrupt storm cannot consume memory;
  - **IO port** and **MMIO range** capabilities, each covering an explicit
    range;
  - a **DMA capability** over a restricted physical range, mapped through the
    IOMMU where one exists.
- **Least privilege is the point** (§7.4): a driver holding the capabilities for
  its own device cannot touch any other device or any memory outside its DMA
  range. This is what makes the Minix-3-style restart story meaningful — a
  crashed driver damaged only its own hardware.
- **The IOMMU caveat from §9.3 applies and must be reported, not assumed.** On
  hardware without a usable IOMMU, DMA capabilities are range-checked at grant
  time but not enforced at access time, and the isolation claim is downgraded.
  The system says so at boot rather than pretending.
- Interrupt acknowledgement is a kernel operation on the IRQ capability, not a
  device register poke, so a driver cannot wedge the interrupt controller for
  everyone.

## Consequences

- Easier: the kernel contains no drivers beyond timer and interrupt controller
  (§5.7); rumpdisk and netdde need glue for capability acquisition rather than
  a device-protocol emulation; the driver isolation claim becomes checkable.
- Harder: anything still using the Mach device interface must be rewritten, not
  ported. The measurement says that set is small, but the boot-path coverage
  gap means it is not certainly empty — re-check after a real boot trace.
- Bootstrap question left to 0013: who holds these capabilities before any
  driver exists, and who hands them out.

## Priority check

Respects the order. Removing a whole interface costs Reuse and buys least
privilege for drivers, which is Resilience — correctly ordered above Reuse.
