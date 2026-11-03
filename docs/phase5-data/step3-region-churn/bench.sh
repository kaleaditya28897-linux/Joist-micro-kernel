#!/bin/sh
# The region churn against requests to mm (Phase 5 step 3; 0008, 0030), on
# the P53's own CPU: mmbench (user/mmbench, mmclient's MMC_BENCH), the
# default build, on 1 and 2 CPUs, 3 boots each, as Phase 4's step7-p53.
# The ISO is built where limine is (the build VM); the boots are made on
# the P53 itself (P53_HOST=aditya@localhost there), with the build VM idle.
# usage: docs/phase5-data/step3-region-churn/bench.sh build OUT  (the build VM)
#        docs/phase5-data/step3-region-churn/bench.sh boot ISO OUTDIR  (the P53)
#        (from the tree's top)
set -u
case ${1:-} in
build)
    cd kernel || exit 1
    make -s -j4 BUILD=build/bench-mm ROOT_TASK=mmbench MODULES="mm mmclient" USER_BUILD=build/mmbench \
        build/bench-mm/joist.iso || { echo BUILD FAILED; exit 1; }
    cp build/bench-mm/joist.iso "$2" ;;
boot)
    iso=$2 out=$3; mkdir -p "$out"
    for s in 1 2; do for i in 1 2 3; do
        log=$out/p53-kvm-smp$s-$i.log
        P53_MEM=256 P53_SMP=$s sh tools/p53/boot.sh "$iso" bios 600 "$log"
        echo "boot $(basename "$log"): status $?"
        grep -a "^mmbench: churn\|root task" "$log"
    done; done ;;
*) echo "usage: bench.sh build OUT | boot ISO OUTDIR" >&2; exit 2 ;;
esac
