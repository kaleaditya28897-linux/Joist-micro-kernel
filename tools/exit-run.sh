#!/bin/sh
# exit-run.sh - Phase 4's exit run (docs/phase4-plan.md, the Testing table's
# exit row; the sizes the owner decided on 2026-10-29, part E3):
#   - cowstress, N rounds on 1, 2 and 4 CPUs under KVM, and N_CHK rounds on
#     the checking kernel (2 CPUs), each with its own seed (COW_SEED = the
#     CPU count);
#   - mm's refinement test with M seeds under plain, asan and ubsan, and
#     M_SLOW under msan and tsan.
# Each part writes its own log in LOGDIR, and a line to LOGDIR/summary.
# It stops at nothing: every part runs, and the status says whether all
# passed. At N = 1000 it took about 55 minutes (2026-10-30): 11 for the
# boots on the P53, the rest for the refinement test on an i7-8665U.
# Give each boot a host CPU per virtual CPU: 4 virtual CPUs squeezed onto 2
# host CPUs make mm miss the 300 ms it is given to answer after a kill.
# cowstress now tells that apart: a slow mm (an answer within 10 s) is
# counted ("slow N (at most ms T)"), and only a stranded one (no answer in
# 10 s) fails the test. But a squeezed run is several times slower (part E7).
# M under 48 fails the refinement test's coverage floors, which are set for
# its default 48 seeds.
#
# usage: tools/exit-run.sh LOGDIR   (from anywhere; needs /dev/kvm, unless
#                                    P53=1, and the toolchain)
# Environment:
#   EXIT_N (default 1000), EXIT_N_CHK (250), EXIT_M (10000), EXIT_M_SLOW (1000)
#   CC   the model's compiler (default clang, as model/Makefile's)
#   P53=1  boot each kernel on the P53's own CPU (tools/p53/boot.sh), as
#          kernel/test/selftest.sh does; the rest runs here
set -u
[ $# -eq 1 ] || { echo "usage: $0 LOGDIR" >&2; exit 2; }
mkdir -p "$1" || exit 2
out=$(cd "$1" && pwd)
top=$(cd "$(dirname "$0")/.." && pwd)
n=${EXIT_N:-1000}
nchk=${EXIT_N_CHK:-250}
m=${EXIT_M:-10000}
mslow=${EXIT_M_SLOW:-1000}
cc=${CC:-clang}
sum=$out/summary
fail=0
note() { # a line in the summary, and on the terminal
    echo "$*" | tee -a "$sum"
}
: > "$sum"
note "exit run: $(date -u '+%Y-%m-%d %H:%M:%S') UTC, $(uname -n), $(git -C "$top" describe --always --dirty 2>/dev/null || echo "no git here")"
note "cowstress N = $n (checking kernel $nchk), refinement M = $m (msan, tsan $mslow)"

# QEMU's status for ISO booted on CPUS CPUs, its serial log in LOG, within
# LIMIT seconds.
boot() { # ISO CPUS LIMIT LOG
    if [ -n "${P53:-}" ]; then
        P53_SMP=$2 "$top/tools/p53/boot.sh" "$1" bios "$3" "$4"
    else
        timeout "$3" qemu-system-x86_64 -M q35 -m 256M -cpu host -enable-kvm -smp "$2" \
            -cdrom "$1" -display none -no-reboot -serial file:"$4" \
            -device isa-debug-exit,iobase=0xf4,iosize=0x04
    fi
}

# cowstress NAME CPUS ROUNDS KEXTRA: built with its own seed, booted, and
# held to what selftest.sh asks of it.
cow() {
    name=$1 cpus=$2 rounds=$3 kextra=$4
    b=build/exit-$name
    log=$out/$name.log
    if ! make -s -C "$top/kernel" BUILD=$b KEXTRA="$kextra" ROOT_TASK=cowstress \
        MODULES="mm cowworker" UEXTRA="-DCOW_ROUNDS=${rounds}u -DCOW_SEED=${cpus}u" \
        USER_BUILD=build/exit-$name $b/joist.iso > "$log.build" 2>&1; then
        note "$name: BUILD FAILED ($log.build)"
        fail=1
        return
    fi
    t0=$(date +%s)
    # A round took 3.5 to 6.2 s on the build VM (E3): 30 s a round is room.
    boot "$top/kernel/$b/joist.iso" "$cpus" $((120 + 30 * rounds)) "$log"
    st=$?
    secs=$(($(date +%s) - t0))
    if [ "$st" -eq 1 ] && grep -a -q "cow: cpus $cpus" "$log" && grep -a -q "cow: all ok" "$log" &&
        grep -a -q "root task exited with code 0" "$log"; then
        note "$name: ok, $rounds rounds, seed $cpus, ${secs} s ($(grep -a -m1 'cow: all ok' "$log" | sed 's/^cow: all ok: //'))"
    else
        note "$name: FAILED (QEMU status $st, $rounds rounds, seed $cpus, ${secs} s; $log)"
        fail=1
    fi
}
cow kvm-smp1-cowstress 1 "$n" ""
cow kvm-smp2-cowstress 2 "$n" ""
cow kvm-smp4-cowstress 4 "$n" ""
cow chk-kvm-smp2-cowstress 2 "$nchk" "-DRUN_CHECK_INVARIANTS"

# The refinement test, SEEDS seeds, in the model's VARIANT build.
refine() { # VARIANT SEEDS
    log=$out/refine-$1.log
    if ! make -s -C "$top/model" CC="$cc" "build/$1/run-tests" > "$log" 2>&1; then
        note "refinement, $1: BUILD FAILED ($log)"
        fail=1
        return
    fi
    t0=$(date +%s)
    JOIST_MM_SEEDS=$2 "$top/model/build/$1/run-tests" mm_refines_the_abstract_machine >> "$log" 2>&1
    st=$?
    secs=$(($(date +%s) - t0))
    # Exactly the one test run, and passed (a filter matching none passes).
    if [ "$st" -eq 0 ] && grep -q "^1 tests, 0 failed" "$log"; then
        note "refinement, $1: ok, $2 seeds, ${secs} s"
    else
        note "refinement, $1: FAILED (status $st, $2 seeds, ${secs} s; $log)"
        fail=1
    fi
}
for v in plain asan ubsan; do refine $v "$m"; done
for v in msan tsan; do refine $v "$mslow"; done

[ $fail -eq 0 ] && note "exit run: all passed" || note "exit run: FAILED (see above)"
exit $fail
