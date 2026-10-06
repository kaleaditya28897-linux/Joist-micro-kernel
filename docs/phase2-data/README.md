# Phase 2 data

Raw evidence behind the numbers in `docs/phase2-plan.md` *Progress*, as for
Phase 1 (`docs/phase1-data/README.md`). Where a raw log is too large or too
binary to be useful in the repository, the summary lines are committed and the
raw log's SHA-256 is recorded here.

## Environment

Workstation: Void Linux, 4 cores. clang 21.1.8, LLD 21.1.8, QEMU 11.1.1
(`qemu-system-x86_64 -M q35 -m 256M`, TCG), Limine 12.9.2, xorriso, all from
Void's packages (installed 2026-10-05).

## Step 7 (2026-10-05): `step7/`

The system-call fuzzer (`user/sysfuzz`) as the root task, 10 000 000 calls per
seed, on the kernel and fuzzer at the commit that adds these files:
`make -C kernel fuzz-kernel SEEDS="11 12 13" CALLS=10000000`. QEMU status 1
(the task exited 0) for all three.

| Seed | Summary | Raw serial log (not committed), bytes | SHA-256 |
|---|---|---:|---|
| 11 | `step7/fuzz-kernel-11.summary.txt` | 2 727 323 | `ac6929bd694b14a6f99425667a12d2dfdecf3d11ede034ee4930b01397daee3f` |
| 12 | `step7/fuzz-kernel-12.summary.txt` | 2 725 599 | `9f0dc545e616dac43a267dba5d02add2e2775255c9a0e51a08df0624ddf64787` |
| 13 | `step7/fuzz-kernel-13.summary.txt` | 2 731 089 | `2ffec2fa79d9705799dadea8cc16a8c0d469132a7605438fe2127b2de99f5e7d` |

Each summary is the raw log with the progress dots removed, filtered to the
fuzzer's start and end lines and the kernel's exit line. The raw logs are
mostly the bytes valid writes printed (0–2 per write, from the task's own
code, stack and dot buffer); a run reproduces them exactly, since the fuzzer
is deterministic per seed.

## VM 112 (2026-10-06): `vm112/`

After Phase 2's acceptance, the same kernel was tested on pvehome's Proxmox
VM 112 (`docs/testbed.md`): a real Intel Core i5-14400 through KVM, rather
than the workstation's QEMU in TCG. Proxmox VE 9.2.20, `pve-qemu-kvm`
11.0.3-3, `pve-edk2-firmware-ovmf` 4.2026.08-1, its bundled SeaBIOS; q35,
`cpu host`, 4 vCPUs pinned to four distinct P-cores, 256 MB. Run with
`tools/pve/boot.sh`.

The kernel gained two lines for this, at the commit that adds these files:
`kernel_finish` prints `joist: finish code N` (Proxmox doesn't pass on
QEMU's exit status, so the script rebuilds it from that line), and the
kernel asks Limine for the other CPUs and reports their count, leaving them
parked (`joist: cpus: 4 found, bsp lapic id 0; 1 in use, ...`).

| Run | Command (from `kernel/`) | Result |
|---|---|---|
| Boot, both firmwares | `make pve-test` | SeaBIOS and OVMF: the root task ran and exited 0 |
| Every self-test | `PVE=1 sh test/selftest.sh` | all 16 pass, in 3 min 23 s |
| The fuzzer, 3 × 100 000 000 calls | `make fuzz-kernel PVE=1 SEEDS="21 22 23" CALLS=100000000` | status 1 (exit 0) for all three, 11 min 18 s in all |
| The `sysret` hazard, guard removed | a one-off build with the guard in `syscall_handler` disabled (`if (0 && ...)`, not committed), `ROOT_TASK=sysrethole`, booted on VM 112 and in local QEMU | see below |

| Seed | Summary | Raw serial log (not committed), bytes | SHA-256 |
|---|---|---:|---|
| 21 | `vm112/fuzz-kernel-21.summary.txt` | 27 258 602 | `e301fada8f3eb0aefe5803943ca905e0c8d2e94342cc870a4b9ae3e931146057` |
| 22 | `vm112/fuzz-kernel-22.summary.txt` | 27 261 373 | `29bc41d9f245eae1d54cc7ec4c159124e474c1883a50d4a930698ed4e7afd2eb` |
| 23 | `vm112/fuzz-kernel-23.summary.txt` | 27 260 559 | `fc68773f25f18b7bd35221336d264b802b37a5a5863e99d23b3b838a61e921e2` |

Seed 21's image was built a few seconds before the CPU-count line was added,
so its summary lacks that line; 22 and 23 have it. The three counts in a
summary add up to about 77% of the calls: zero-length writes are checked
(they must return 0) but counted in none of the three, as in step 7.

**The `sysret` hazard on Intel.** With the guard removed, VM 112 reports
`EXCEPTION 13 (#GP general protection) in the kernel` with `cs 0x8` (ring
0), `rsp 0x400000` (the user's stack pointer) and `rcx 0x800000000000`
(`vm112/sysret-noguard-intel.txt`): the exploitable form, a fault taken in
ring 0 on a user-controlled stack, which step 7 could not show because TCG
raises it in ring 3 as AMD CPUs do (`vm112/sysret-noguard-tcg.txt`, same
image: `root task fault: #GP ... at 0x0000800000000000`). With the guard, the
self-test `sysrethole` passes on VM 112: the kernel refuses the return before
`sysret`.
