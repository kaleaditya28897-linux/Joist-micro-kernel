# Phase 3 data

Raw evidence behind the numbers in `docs/phase3-plan.md` *Progress*, as for
Phases 1 and 2. Environments are in `docs/testbed.md`: `joist-dev` (VM 113
on pve1) builds and runs every host test and the QEMU (TCG) boots; VM 112 on
pvehome boots images on a real Intel CPU through KVM.

## Step 4 (2026-10-10): `step4/`

The step's goal on VM 112: the root task loads `pong` and `ping` from boot
modules and they make 200 000 round trips across address spaces. Image:
`kernel/build/selftest-pingpong-long/joist.iso` built on joist-dev by
`sh test/selftest.sh` (`ROOT_TASK=root MODULES="pong ping"
UEXTRA=-DPING_ROUNDS=200000u`) at the commit that adds these files, SHA-256
`76d621b345bc1b748847a9e9fc22b601a42907890236eee8a3a5442395ae17fc`; booted with
`tools/pve/boot.sh ISO bios|uefi 120 LOG`.

| Firmware | Serial log (from `joist: boot`) | Result |
|---|---|---|
| SeaBIOS | `step4/vm112-pingpong-long-bios.txt` | status 1 (the root task exited 0); 200 000 round trips; 33 timer ticks at exit, 20 of them before the root task starts |
| OVMF | `step4/vm112-pingpong-long-uefi.txt` | the same; 35 ticks |

So about 13 to 15 ticks (130 to 150 ms) for 200 000 round trips: roughly
0.65 to 0.75 µs each under KVM on the i5-14400. This is the 100 Hz tick
counter, not a benchmark (step 8 measures with the TSC); it is recorded as
the first evidence that IPC on real hardware is in the expected range.
