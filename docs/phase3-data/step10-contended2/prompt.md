You are working alongside another Claude Code session on the Joist microkernel
project. Measurements only: change no source file outside your own copy,
commit nothing, change no system setting (no sudo).

Background. Your last report (/srv/joist/handoff/contend/findings.md) found
the busy CPUs' holds were 43 % scheduler. The kernel now skips the scheduler
after a call that changes nothing (commit 8241b56, joist_sched_stays in
kernel/core/sched.c, its use in kernel/arch/x86_64/run.c). Result of a
20-boot campaign of the bench-gate image (booted exactly as
boot.sh.current in this directory: guest RAM a zeroed /dev/shm file, QEMU on
CPUs 0,1,6,7, the four vCPUs moved to 2-5):
- every uncontended line passes (spreads 0-1.6 % median);
- the do-nothing call fell 528 -> 412, and the contended line ("two address
  spaces, passive server, every other CPU making system calls") fell from
  9 232 to 6 864 cycles median, BUT its median now spreads 12.1 % across
  boots (6 480-7 312), and within one boot its 20 rounds disagree too
  (pooled p50 6 480, worst round's p50 7 008; min about 4 600).
The plan requires the median to move < 5 % (p99 < 20 %) in every 10-run
window, for every line. The other session will change the kernel (or, only
with a sound reason, the benchmark) on what you find.

Hypothesis to test: before the change, the three busy CPUs' long holds kept
the ticket lock saturated (a steady convoy); now their holds are short, the
lock is near saturation, and how often the client's two acquisitions per
round trip wait behind 0-3 others depends on the loops' phases, which
settle differently per round and per boot.

Tree: on the build VM (ssh -p 2222 aditya@127.0.0.1; the name joist-dev2
does not resolve from the P53), ~/joist-ct2 is a clean copy of the current
tree (8241b56). Work only there. Your lock-stats instrumentation
(RUN_LOCK_STATS, BENCH_LOCK_STATS) is in it. Reuse your tools in
/srv/joist/handoff/contend/tools (check.sh, campaign.sh, analyze.py), but
boot as boot.sh.current does (fixed RAM file, the taskset dance); check the
host settings before and after as before, and stop if they differ. Keep the
build VM otherwise idle while booting; nothing else should run on it.

Do (10 boots per image unless noted, interleaved where you compare):
1. Per-round data: in your copy, make ipcbench (behind a UEXTRA flag) also
   print, for the contended line, each of the 20 rounds' p50 and a coarse
   histogram of all samples (e.g. 128-cycle buckets from min to p99.9).
   Is the distribution multi-modal? Do the modes stay put while their
   weights move between rounds and boots?
2. A RUN_LOCK_STATS build: per boot, for the contended window: lock
   utilisation (held time / wall time), the busy CPUs' and the client's
   mean/p99 hold and wait, and the waiting share. Correlate with p50.
3. The saturation hypothesis directly: variants where each busy CPU spins
   a fixed N TSC counts in user mode between its calls (N = 0, 300, 1000,
   3000, 10000; flag in your copy of ipcbench only), 10 boots each: the
   contended p50, its spread across boots and the worst round's p50. Does
   the spread peak near some lock utilisation and fall away from it?
4. Also with 1 and 2 busy CPUs instead of 3 (N = 0), 10 boots each.
Report in /srv/joist/handoff/contend2/findings.md: a short answer first,
then tables (per boot), the method, images' SHA-256; raw logs in raw/;
your patches in tools/. Status: append one line ending with "done" to
/srv/joist/handoff/contend2/status (the run.sh wrapper writes the rest).
Budget: aim to finish within about 2 hours of boots; report what you have
if you must cut items 3-4 short, and say so.
