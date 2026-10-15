#!/bin/sh
# build.sh: run on the build VM, in ~/joist-to/kernel. Builds every image
# of the timer-out study; prints sha256 lines.
set -e
cd ~/joist-to/kernel
mk() { # NAME KEXTRA ROOT MODULES UEXTRA USER_BUILD
    make -s BUILD=build/$1 KEXTRA="$2" ROOT_TASK=$3 MODULES="$4" UEXTRA="$5" \
        USER_BUILD=$6 build/$1/joist.iso >build-$1.log 2>&1 || { echo "BUILD FAILED $1"; cat build-$1.log; exit 1; }
}
mk to-new-sc "" scaleroot ipcbench "" build
mk to-old-sc "-DRUN_LOCK_TIMER_OUT=0" scaleroot ipcbench "" build
mk to-new-lb "" benchroot ipcbench "" build
mk to-old-lb "-DRUN_LOCK_TIMER_OUT=0" benchroot ipcbench "" build
mk to-smp "" smproot worker "" build
mk to-sched "" schedroot worker "" build
for s in $(seq 21 30); do
    mk fi-$s "" faultinj fiworker "-DFAULTINJ_SEED=$s" build/faultinj-$s
    mk fichk-$s "-DRUN_CHECK_INVARIANTS" faultinj fiworker "-DFAULTINJ_SEED=$s" build/faultinj-$s
done
cd build && sha256sum */joist.iso
