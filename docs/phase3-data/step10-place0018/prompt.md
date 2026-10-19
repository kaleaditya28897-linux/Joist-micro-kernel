You are working alongside another Claude Code session on the Joist microkernel
project. Measurements only: change no source file outside your own copy,
commit nothing, change no system setting (no sudo).

Background. Your study /srv/joist/handoff/contend2/findings.md found that
about 47 % of the contended line's boot-to-boot variance was which vCPU the
client landed on (the idle-CPU wake race). The owner chose decision 0018
(docs/decisions/0018-wake-placement.md in the tree below): one CPU woken per
thread made ready, idle CPUs first, lowest first, so placement should
follow the order of events, not a race. It is implemented and verified
(joist_sched_pokes in kernel/core/sched.c; poke_others in
kernel/arch/x86_64/run.c).

Result of a 40-boot interleaved campaign (analysis-0018.txt here; "a" =
8241b56, "b" = 0018; booted exactly as boot.sh.current here, which now
also narrows the build VM to 0,1,6,7 for each boot): every uncontended line
passes; the contended line's median ("two address spaces, passive server,
every other CPU making system calls") is 7 056 for a (7.9 %) and 7 096 for
b (11.3 %; worst 10-window 10.0 %; b's boot p50s 6 944-7 424 plus one at
6 624). The criterion is < 5 % in every 10-run window. So 0018 did not
help.

Questions, in order:
1. Under 0018, is the client's vCPU (and each busy thread's) now the same
   in every boot? Use your RUN_LOCK_DEPTH / placement instrumentation from
   contend2 (tools/joist-ct2.patch), ported to the tree ~/joist-ct3 on the
   build VM (ssh -p 2222 aditya@127.0.0.1), which is a clean copy of the
   0018 tree. 20 boots are enough.
2. If placement is fixed: what now explains the boot-to-boot spread? Test
   at least the per-boot host placement (fixed /dev/shm RAM is already in
   boot.sh) and whether the fixed placement is a bad or good pair of cores
   (the 1-busy study showed pairs differing by up to 32 %). Is the spread
   within one placement the residual 1.7 % SD, or larger?
3. If placement is not fixed: why not (e.g. the benchmark's threads start
   in an order that varies, or the client is woken while some CPU is busy),
   with the evidence from the logs.
4. Against the criterion: with what you find, is there a kernel-side
   change that would plausibly get the line under 5 %, or is the remainder
   host-side? Observations, not recommendations of a decision.

Rules: the build VM must be idle during boots (your campaign script's 2 %
gate); boot only through boot.sh.current's command line (fixed RAM, QEMU on
0,1,6,7, vCPUs on 2-5, the build VM narrowed). Budget about 90 minutes of
boots. Write /srv/joist/handoff/place0018/findings.md (short answers first,
then method and tables), keep raw logs in raw/ and your scripts in tools/.
