#!/bin/sh
# Decision 0032's copies against the IPC buffer's (Phase 5 step 9), on the
# P53's own CPU: ipcbench (user/ipcbench, benchroot) with its reply_write
# measurements of 4, 16 and 64 KiB, the default build, on 1 and 2 CPUs, 3
# boots each, as step 3's region churn (docs/phase5-data/step3-region-churn/).
# The ISO is built where limine is (the build VM); the boots are made on the
# P53 itself (P53_HOST=aditya@localhost there), with the build VM idle.
# usage: docs/phase5-data/step9-copy/bench.sh build OUT  (the build VM)
#        docs/phase5-data/step9-copy/bench.sh boot ISO OUTDIR  (the P53)
#        (from the tree's top)
set -u
case ${1:-} in
build)
    cd kernel || exit 1
    make -s -j4 BUILD=build/bench-copy ROOT_TASK=benchroot MODULES=ipcbench \
        build/bench-copy/joist.iso || { echo BUILD FAILED; exit 1; }
    cp build/bench-copy/joist.iso "$2" ;;
boot)
    iso=$2 out=$3; mkdir -p "$out"
    for s in 1 2; do for i in 1 2 3; do
        log=$out/p53-kvm-smp$s-$i.log
        P53_MEM=256 P53_SMP=$s sh tools/p53/boot.sh "$iso" bios 900 "$log"
        echo "boot $(basename "$log"): status $?"
        grep -a "^ipcbench:" "$log" | sed 's/ worst-p50.*//'
    done; done ;;
*) echo "usage: bench.sh build OUT | boot ISO OUTDIR" >&2; exit 2 ;;
esac
