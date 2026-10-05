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
