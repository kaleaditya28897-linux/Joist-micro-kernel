# The IPC benchmark with the FPU kept per thread (Phase 5 step 7)

The kernel now saves each thread's FPU and SSE state (FXSAVE, 512 bytes) on
every entry from user mode and loads it when a CPU switches to another
thread (the owner's choice, 2026-11-07: eager FXSAVE, SSE only;
`docs/abi.md`). A round trip switches twice, so it pays two saves and two
loads more, whether or not either thread uses the FPU.

Measured with the bench-gate entry of `kernel/test/selftest.sh`, medians in
cycles by ipcbench's ruler:

| | before (baseline, 2026-10-15) | with the FPU (2026-11-07) |
|---|---|---|
| a system call that does nothing | 492-496 | 444 |
| two address spaces, passive server | 1548 | 1896-1900 |
| one address space, passive server | 1088-1092 | 1424 |

- After: 10 runs on the P53's Intel Core i7-9850H, through KVM in QEMU
  10.0.13 in the build VM (8 vCPUs), nothing else running, at commit
  e486daa (`p53-bench-gate-10-runs.txt`, as printed).
- The same kernel with the save and load taken out, on the laptop (Intel
  Core i7-8665U, KVM), gave 1612 and 1144 for the round trips, with the FPU
  1904 and 1424: about 290 cycles a round trip are the FPU's. The null
  system call is within its usual spread.

The one-address-space median was then 30% above the old baseline, past the
gate's 25%. The owner accepted the cost and a new baseline (2026-11-07):
the round trips' medians above; the system call's stays at 496, which the
FPU doesn't move and a tighter bound would fail on its spread (380-696 on
the laptop, busy);
cheaper switching (XSAVEOPT, or threads that never use the FPU) would be a
decision of its own.
