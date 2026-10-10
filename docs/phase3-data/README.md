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

## Step 5 (2026-10-10): `step5/`

The scheduler on VM 112, at the commit that adds these files. Images built
on joist-dev by `make check` in `kernel/` (`sh test/selftest.sh`), booted
with `tools/pve/boot.sh ISO bios|uefi 120 LOG`:

| Image | Built as | SHA-256 |
|---|---|---|
| `kernel/build/selftest-sched/joist.iso` | `ROOT_TASK=schedroot MODULES=worker` | `b9598017a05745223c5c73813ef8d64f2124f436447ad0e3364b72175bb57fd1` |
| `kernel/build/selftest-pingpong-long/joist.iso` | `ROOT_TASK=root MODULES="pong ping" UEXTRA=-DPING_ROUNDS=200000u` | `63b53c9fa3329c148a603e3ac30eb7f195a00abf8decf7fc1bcb79d1f8c93786` |

| Run | Serial log (from `joist: boot`) | Result |
|---|---|---|
| Scheduler test, SeaBIOS | `step5/vm112-sched-bios.txt` | status 1 (exited 0). Budget of 2 ms in 10: 20 %. Spinner beside system calls on that budget keeps 81 %. Equals: 50 %. Passive server on its client's budget: 19 % (1 193 calls); the spinner beside keeps 80 % |
| Scheduler test, OVMF | `step5/vm112-sched-uefi.txt` | status 1. 19 %, 79 %, 50 %, 19 % (1 196 calls), 80 % |
| Ping-pong, 200 000 round trips, SeaBIOS | `step5/vm112-pingpong-long-bios.txt` | status 1; 9 timer interrupts after boot |
| Ping-pong, OVMF | `step5/vm112-pingpong-long-uefi.txt` | status 1; 13 |

Each line of the scheduler test prints the two counts behind its
percentage. In the ping-pong run a timer interrupt now marks the end of a
10 ms budget (the timer is one-shot), so 9 to 13 of them is roughly 90 to
130 ms for 200 000 round trips: 0.45 to 0.65 µs each, where step 4 measured
0.65 to 0.75 µs by the same rough method. Still not a benchmark (step 8).

Two earlier boots of this step are described in `docs/phase3-plan.md`
(*Step 5*, parts 4 and 5) and not kept as files: the kernel before kernel
time was charged, which fails the scheduler test on this VM; and the kernel
that wrote the timer on every exit, on which the same ping-pong took 51
interrupts (about 2.5 µs a round trip).

## Step 6 (2026-10-10): `step6/`

The fault test on VM 112, at the commit that adds these files. Image
`kernel/build/selftest-fault/joist.iso` (`ROOT_TASK=faultroot
MODULES=faulter`), built on joist-dev by `make check` in `kernel/`, SHA-256
`8940134ec50f1afe0f4d32dee69446a80067415ed6f48dc234f69455fd762e41`; booted
with `tools/pve/boot.sh ISO bios|uefi 120 LOG`.

| Firmware | Serial log (from `joist: boot`) | Result |
|---|---|---|
| SeaBIOS | `step6/vm112-fault-bios.txt` | status 1 (the root task exited 0): all six tests |
| OVMF | `step6/vm112-fault-uefi.txt` | the same |

