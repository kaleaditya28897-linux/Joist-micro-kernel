#!/bin/sh
# selftest.sh - boot test builds of the kernel that misbehave on purpose and
# check it reports each case correctly (Phase 2 steps 2, 4, 6, 7 and 8). Each variant is
# built with -DJOIST_SELFTEST=N in its own build directory and booted under
# BIOS; QEMU's exit status is (kernel code << 1) | 1.
# usage: test/selftest.sh   (from kernel/)
#        P53=1 test/selftest.sh   boot each on the P53's own CPU instead
#                                 (../tools/p53/boot.sh, docs/testbed.md)
fail=0
boot() { # ISO LOG: boot it, return QEMU's exit status
    if [ -n "${P53:-}" ]; then
        ../tools/p53/boot.sh "$1" bios 60 "$2"
    else
        timeout ${QEMU_TIMEOUT:-60} qemu-system-x86_64 -M q35 -m 256M ${cpu:+-cpu $cpu} ${smp:+-smp $smp} -cdrom "$1" -display none -no-reboot \
            -serial file:"$2" -device isa-debug-exit,iobase=0xf4,iosize=0x04
    fi
}
run() { # N EXPECTED_STATUS "grep pattern"...   (N numeric: a kernel selftest
       #  build; N a name: the normal kernel with user/N as the root task)
    n=$1; want=$2; shift 2
    ub="-fsanitize=undefined -fsanitize-trap=undefined"
    mods=""; uextra=""; ubuild=build; ksizes=""; cpu=""; smp=""
    chk="-DRUN_CHECK_INVARIANTS"
    # Phase 3 step 9: NAME on 2 or 4 CPUs (smp2-NAME, smp4-NAME); with
    # "kvm" before the count, on the real CPU's (kvm-smp4-NAME), where
    # they truly run at once; with "chk-" first, on a kernel that checks
    # every invariant before every exit to user mode.
    full=$n
    case $n in chk-*) n=${n#chk-}; ;; esac
    case $n in kvm-smp[24]-*) cpu="host -enable-kvm"; n=${n#kvm-} ;; esac
    case $n in smp[24]-*) smp=${n%%-*}; smp=${smp#smp}; n=${n#smp[24]-} ;; esac
    # One build per kernel and root task, whatever machine it is booted on;
    # one log per run.
    case $full in chk-*) b=build/selftest-chk-$n ;; *) b=build/selftest-$n ;; esac
    log=$b/serial.log
    [ "$full" = "$n" ] || log=$b/serial-$full.log
    big="-DJOIST_MAX_OBJECTS=8192u -DJOIST_MAX_MAPPINGS=512u -DJOIST_MAX_CSPACES=64u -DROOT_BUDGET=16384u"
    case $n in
    # Phase 3 step 4: the root task loads pong and ping from boot modules.
    pingpong) kextra=""; root=root; mods="pong ping" ;;
    pingpong-long) kextra=""; root=root; mods="pong ping"; uextra="-DPING_ROUNDS=200000u"; ubuild=build/long ;;
    pingpong-buf) kextra=""; root=root; mods="pong ping"; uextra="-DPING_WORDS=512u -DPING_ROUNDS=20000u"; ubuild=build/buf ;;
    ubsan-pingpong) kextra="$ub"; root=root; mods="pong ping" ;;
    # Phase 3 step 5: the scheduler test's root task and its workers.
    sched) kextra=""; root=schedroot; mods="worker" ;;
    ubsan-sched) kextra="$ub"; root=schedroot; mods="worker" ;;
    # Phase 3 step 7: the destruction test, and the same on a kernel built
    # never to preempt a cascade, which must fail it.
    # All three with larger tables than the default kernel's, so that a
    # cascade is long enough to measure against ($big).
    cascade) kextra=""; root=cascaderoot; mods="worker"; ksizes=$big ;;
    ubsan-cascade) kextra="$ub"; root=cascaderoot; mods="worker"; ksizes=$big ;;
    cascade-nopreempt) kextra="-DRUN_PREEMPT_SLOTS=0u"; root=cascaderoot; mods="worker"; ksizes=$big ;;
    # Phase 3 step 8: the same tests on the real CPU through KVM, which has
    # PCID (QEMU's emulated CPUs have none): there a switch of address space
    # keeps the TLB. Skipped, and said so, where there is no /dev/kvm.
    pcid-fault) kextra=""; root=faultroot; mods="faulter"; cpu="host -enable-kvm" ;;
    pcid-pingpong) kextra=""; root=root; mods="pong ping"; cpu="host -enable-kvm" ;;
    pcid-cascade) kextra=""; root=cascaderoot; mods="worker"; ksizes=$big; cpu="host -enable-kvm" ;;
    # Phase 3 step 8: the IPC benchmark. Under emulation it only has to run;
    # on the real CPU (KVM) its medians are held against the baseline.
    bench) kextra=""; root=benchroot; mods="ipcbench" ;;
    bench-gate) kextra=""; root=benchroot; mods="ipcbench"; cpu="host -enable-kvm" ;;
    # Phase 3 step 6: the fault test's root task and its faulters.
    fault) kextra=""; root=faultroot; mods="faulter" ;;
    ubsan-fault) kextra="$ub"; root=faultroot; mods="faulter" ;;
    smp) kextra=""; root=smproot; mods="worker" ;;
    # Phase 3's exit: the memory-growth test (fewer rounds under emulation).
    grow) kextra=""; root=growroot; mods="fuzzchild"; uextra="-DGROW_ROUNDS=8u"; ubuild=build/grow8 ;;
    grow40) kextra=""; root=growroot; mods="fuzzchild" ;;
    [0-9]*) kextra="-DJOIST_SELFTEST=$n"; root=hello ;;
    ubsan-[0-9]*) kextra="$ub -DJOIST_SELFTEST=${n#ubsan-}"; root=hello ;;
    ubsan-*) kextra="$ub"; root=${n#ubsan-} ;;
    *) kextra=""; root=$n ;;
    esac
    case $full in chk-*) kextra="$kextra $chk" ;; esac
    n=$full
    case $cpu in *enable-kvm*)
        [ -w /dev/kvm ] || { echo "selftest $n: SKIPPED (no /dev/kvm here; it needs the real CPU)"; return; } ;;
    esac
    make -s BUILD=$b KEXTRA="$kextra" ROOT_TASK=$root MODULES="$mods" UEXTRA="$uextra" \
        USER_BUILD=$ubuild ${ksizes:+KSIZES="$ksizes"} $b/joist.iso >/dev/null 2>&1 ||
        { echo "selftest $n: BUILD FAILED"; fail=1; return; }
    boot $b/joist.iso $log
    st=$?
    ok=1
    [ "$st" -eq "$want" ] || ok=0
    for p in "$@"; do grep -a -q -- "$p" $log || ok=0; done
    if [ $ok -eq 1 ]; then
        echo "selftest $n: ok ($(grep -a -m1 -E 'EXCEPTION|UBSAN|#BP|ping: [0-9]+ round|STACK SMASHED|sched: all ok|fault: all ok|cascade: (all ok|FAILED)|ipcbench: done|smp: (all ok|one CPU)|grow: all ok|root task (fault|exited|refused)|refused|sysfuzz: done' $log | tr -d '.' | sed 's/^joist: //'))"
    else
        echo "selftest $n: FAILED (QEMU status $st, wanted $want)"; cat $log; fail=1
    fi
}
run 1 5 "EXCEPTION 6 (#UD invalid opcode)" "rip 0xffffffff8"
run 2 5 "EXCEPTION 14 (#PF page fault)" "cr2 0xffff900000000000"
run 3 1 "#BP breakpoint at 0xffffffff8" "joist: ok"
# Step 4: the kernel's own page tables enforce W^X. Error codes: 0x3 = present
# page, write; 0x11 = present page, instruction fetch.
run 4 5 "EXCEPTION 14 (#PF page fault)" "error 0x0000000000000003"
run 5 5 "EXCEPTION 14 (#PF page fault)" "error 0x0000000000000011"
# Step 6: user mode. Root task exits with 0: status 1; root task fault: 9.
run hello 1 "hello from user mode" "root task exited with code 0"
run badptr 1 "badptr: refused as expected" "root task exited with code 0"
run ud 9 "root task fault: #UD invalid opcode at 0x0000000000400004"
run rowrite 9 "root task fault: #PF page fault at" "address 0x0000000000401000"
run spin 1 "spin: done" "root task exited with code 0"
# Step 7: the system-call fuzzer, default seed, 1 000 000 calls.
run sysfuzz 1 "sysfuzz: done" "registers preserved" "root task exited with code 0"
# The sysret hazard: a syscall in the last two bytes of the user half must be
# refused (status 7: code 3), not sysret to 0x800000000000.
run sysrethole 7 "syscall returning to non-canonical 0x0000800000000000"
# Step 8: the double fault arrives on its own stack (IST1).
run 7 5 "EXCEPTION 8 (#DF double fault)"
# Phase 3's exit: a kernel stack that overflows hits its guard page (the
# fault, with no stack to be taken on, becomes the double fault); and the
# stack protector catches a write past a protected function's array.
run 8 5 "selftest: kernel stack overflow" "EXCEPTION 8 (#DF double fault)"
run 9 5 "selftest: stack smash" "STACK SMASHED"
# Step 8: the kernel's UBSan build (trap mode): a deliberate signed overflow is
# named, and the normal root task and the fuzzer run clean under it.
run ubsan-6 5 "UBSAN: signed add overflow (kind 0)"
run ubsan-hello 1 "hello from user mode" "root task exited with code 0"
run ubsan-sysfuzz 1 "sysfuzz: done" "registers preserved"
# Phase 3 step 4: three tasks. The root task loads pong and ping from boot
# modules into address spaces of their own and hands them their endpoints;
# ping calls pong 1000 times and reports. The long run is preempted by the
# timer hundreds of times in mid-IPC; the UBSan kernel runs it too.
run pingpong 1 "root: pong and ping started" "ping: 1000 round trips ok" \
    "root: ping reported 1000 round trips" "root task exited with code 0"
run pingpong-long 1 "ping: 200000 round trips ok" "root: ping reported 200000 round trips"
run ubsan-pingpong 1 "ping: 1000 round trips ok" "root task exited with code 0"
# Phase 3 step 5: the scheduler, seen from user mode (user/schedroot): fixed
# priorities and preemption on waking, a budget enforced per period (in user
# mode and in system calls), equals taking turns, a passive server working on
# its client's budget.
run sched 1 "sched: priorities ok" "sched: budget ok" "sched: kernel time ok" "sched: equals ok" \
    "sched: donation ok" "sched: all ok" "root task exited with code 0"
run ubsan-sched 1 "sched: all ok" "root task exited with code 0"
# Phase 3 step 6: fault IPC and thread control, seen from user mode
# (user/faultroot): a fault is a message to the handler, who reads and writes
# the thread's registers and resumes or ends it; a page fault resolved by
# mapping; no handler destroys the task; an unanswered fault leaves the
# thread suspended; suspend, resume, kill, abort, the TLS base.
run fault 1 "fault: resume ok" "fault: pager ok" "fault: terminate ok" \
    "faulted (#DE divide error at 0x00000000004[0-9a-f]*) with no handler: destroyed" \
    "faulted (#UD invalid opcode at 0x00000000004[0-9a-f]*) with no handler: destroyed" \
    "fault: no handler ok" "fault: unresolved ok" "fault: control ok" "fault: all ok" \
    "root task exited with code 0"
run ubsan-fault 1 "fault: all ok" "root task exited with code 0"
# Phase 3 step 7: destroying a chain of tasks with full capability tables
# (user/cascaderoot) doesn't keep a higher priority waiting, and everything
# comes back. On a kernel that sweeps a cascade in one piece the same test
# fails (status 7: the root task exits 1), which shows it can tell.
run cascade 1 "cascade: preemption ok" "cascade: all ok" "root task exited with code 0"
run ubsan-cascade 1 "cascade: all ok" "root task exited with code 0"
run cascade-nopreempt 7 "cascade: FAILED: a higher priority waited for a whole cascade"
# Phase 3 step 8: with PCID (the real CPU, through KVM), where switching
# address spaces no longer flushes the TLB: the fault test (a page unmapped
# from a task that isn't running must fault again), ping-pong, and the
# cascade (spaces destroyed and their numbers reused).
run pcid-fault 1 "address spaces: PCID in use" "fault: pager ok" "fault: all ok"
run pcid-pingpong 1 "address spaces: PCID in use" "ping: 1000 round trips ok"
run pcid-cascade 1 "address spaces: PCID in use" "cascade: all ok"
# Phase 3 step 8: the IPC benchmark runs (four measurements, each in one
# line), and on the real CPU no median is more than 25% above its baseline
# (test/ipcbench.baseline): the regression gate.
run bench 1 "ipcbench: a system call that does nothing: n=" "ipcbench: two address spaces, passive server: n=" \
    "ipcbench: one address space, passive server: n=" "ipcbench: done" "root task exited with code 0"
run bench-gate 1 "address spaces: PCID in use" "ipcbench: done"
if [ -f build/selftest-bench-gate/serial.log ] && [ -w /dev/kvm ]; then
    grep -v '^#' test/ipcbench.baseline | grep '|' | while IFS='|' read -r what base; do
        got=$(grep -a "ipcbench: $what: " build/selftest-bench-gate/serial.log | sed -n 's/.* p50=\([0-9]*\) .*/\1/p')
        if [ -z "$got" ] || [ "$got" -gt $((base + base / 4)) ]; then
            echo "bench gate: FAILED: $what: median ${got:-missing}, baseline $base (limit $((base + base / 4)))"
            echo 1 > build/selftest-bench-gate/failed
        else
            echo "bench gate: ok: $what: median $got cycles (baseline $base)"
        fi
    done
    [ -f build/selftest-bench-gate/failed ] && { rm -f build/selftest-bench-gate/failed; fail=1; }
fi
# Phase 3 step 9: more than one CPU (decision 0016: one kernel lock). The
# multiprocessor test itself, then the tests above again on 2 and 4 CPUs
# (those written for one CPU keep the others busy: user/lib/testroot.h),
# emulated and on the real CPU; and on a kernel that checks the core's
# invariants before every exit to user mode, which is where a race between
# CPUs would show.
run smp 1 "smp: one CPU: nothing to test" "root task exited with code 0"
for ncpu in 2 4; do
    run smp$ncpu-smp 1 "smp: $ncpu CPUs" "smp: parallel ok" "smp: priorities ok" "smp: ipc ok" \
        "smp: control ok" "smp: destroy ok" "smp: all ok"
    run kvm-smp$ncpu-smp 1 "cpus in use: $ncpu" "smp: all ok"
    run smp$ncpu-hello 1 "cpus in use: $ncpu" "hello from user mode" "root task exited with code 0"
    run smp$ncpu-pingpong 1 "ping: 1000 round trips ok" "root: ping reported 1000 round trips"
    run smp$ncpu-sysfuzz 1 "sysfuzz: done" "registers preserved" "root task exited with code 0"
    run smp$ncpu-fault 1 "cpus in use: $ncpu" "fault: all ok"
    run kvm-smp$ncpu-fault 1 "cpus in use: $ncpu" "fault: all ok"
    run smp$ncpu-sched 1 "cpus in use: $ncpu" "sched: all ok"
    run smp$ncpu-cascade 1 "cpus in use: $ncpu" "cascade: all ok"
    run kvm-smp$ncpu-cascade 1 "cpus in use: $ncpu" "cascade: all ok"
done
# (The checking kernel on the real CPU only: emulated, it is too slow for
# the tests' own time limits. And not the tests that measure shares of
# time: the check, at every exit, is time too.)
run chk-kvm-smp4-smp 1 "smp: all ok"
run chk-kvm-smp4-fault 1 "fault: all ok"
run chk-kvm-smp2-pingpong 1 "ping: 1000 round trips ok"
run chk-kvm-smp4-pingpong 1 "ping: 1000 round trips ok"
# Phase 3's exit: the IPC buffer. Ping sends its whole buffer (512 words)
# with every call and gets it back complemented, between two address
# spaces: on one CPU, on 4 real ones, and on the checking kernel.
run pingpong-buf 1 "ping: 20000 round trips ok, 512 words of the IPC buffer each way each time" \
    "root task exited with code 0"
run kvm-smp4-pingpong-buf 1 "ping: 20000 round trips ok, 512 words"
run chk-kvm-smp2-pingpong-buf 1 "ping: 20000 round trips ok, 512 words"
# Phase 3's exit: no kernel memory growth (user/growroot). Rounds of tasks,
# threads, objects, capabilities and mappings made and destroyed, every
# fixed table filled to E_NOMEM, cycles of tasks reaped, children making
# system calls at random; the kernel's counters (debug_counters) must be
# where they were after the first round, every round. On one emulated CPU
# (8 rounds), on 4 real ones (40), and on the checking kernel.
run grow 1 "grow: all ok" "root task exited with code 0"
run kvm-smp4-grow40 1 "cpus in use: 4" "grow: all ok"
run chk-kvm-smp4-grow 1 "grow: all ok"
exit $fail
