# Step 3: the fault round trip (2026-10-28)

`user/faultroot`'s `test_bench` (commit 789bc99): a `faulter` touches 256
fresh pages of one last-level table, one fault each; `faultroot`, its
handler, receives each fault, maps a page there, replies, and deletes the
thread capability if the fault brought one. The faulter times each round
trip with the TSC (`lfence; rdtsc` both sides). Two runs per boot: with the
thread capability, and with `JOIST_FAULT_NO_THREAD_CAP`.

## On the P53, through KVM (`tools/p53/boot.sh`, pinned cores)

The `selftest-fault` ISO of the kernel check, booted by hand: `P53_SMP=1`
on host CPU 2, `P53_SMP=2` on CPUs 2 and 3. Logs: `p53-kvm-smp*.log`.

| | median, with | fastest, with | median, without | fastest, without |
|---|---:|---:|---:|---:|
| 1 CPU, run 1 | 10 838 | 9 690 | 10 522 | 9 166 |
| 1 CPU, run 2 | 11 212 | 9 778 | 10 404 | 9 246 |
| 1 CPU, run 3 | 10 952 | 9 816 | 11 420 | 9 264 |
| 2 CPUs, run 1 | 25 888 | 11 158 | 25 022 | 19 582 |
| 2 CPUs, run 2 | 25 738 | 10 972 | 25 394 | 19 872 |
| 2 CPUs, run 3 | 25 546 | 10 568 | 25 338 | 10 906 |

## On the build VM (nested KVM), for comparison

From the P53's Claude Code (`/srv/joist/handoff/p4-fault/findings.md`):
one CPU 61 038 / 60 202; two CPUs, mean of six medians 135 645 / 135 441;
four CPUs 136 052 / 135 874. Nested virtualisation multiplies these about
six times, so they are not figures for the fault path.

## What they say

- Leaving out the thread capability saves nothing measurable (a few per
  cent either way, run to run).
- One CPU: about 11 000 cycles a fault, 9 200 at best.
- Two CPUs: the median doubles (about 25 500) while the fastest stays
  near the one-CPU figure: some faults take a slower path, likely a
  wake-up across CPUs. Not yet explained.
