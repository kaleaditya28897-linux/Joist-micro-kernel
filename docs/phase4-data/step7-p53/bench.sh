#!/bin/sh
# mmbench on the P53's own CPU (tools/p53/boot.sh: KVM, virtual CPUs pinned
# to host CPUs 2-5), with E2's and E6's recipe: the default build on 1 and 2
# CPUs, 3 boots each; the large-clone build on 1 CPU, 3 boots each.
# usage: docs/phase4-data/step7-p53/bench.sh OUTDIR   (from the tree's top)
set -u
out=$1; mkdir -p "$out"
top=$(pwd)
cd kernel || exit 1
make -s -j4 BUILD=build/bench-mm ROOT_TASK=mmbench MODULES="mm mmclient" USER_BUILD=build/mmbench \
    build/bench-mm/joist.iso || { echo BUILD FAILED; exit 1; }
make -s -j4 BUILD=build/bench-mm-big ROOT_TASK=mmbench MODULES="mm mmclient" USER_BUILD=build/mmbench-big \
    UEXTRA="-DMMB_POOL=66048u -DMMB_CLONE_PAGES=65536u -DMMB_TABLES=288u" \
    MMFLAGS="-DMM_MAX_TASKS=4u -DMM_TASK_PAGES=66048u -DMM_PAGES=66048u -DMM_TABLE_PAGES=288u" \
    KEXTRA="-DRUN_POOL_FRAMES=2u -DRUN_POOL_PAGES=69632u -DROOT_BUDGET=16384u" \
    KSIZES="-DJOIST_MAX_OBJECTS=8192u -DJOIST_MAX_CSPACES=64u -DJOIST_MAX_THREADS=128u -DJOIST_MAX_LEAVES=256u" \
    build/bench-mm-big/joist.iso || { echo BIG BUILD FAILED; exit 1; }
boot() { # ISO LOG MEM SMP
    P53_MEM=$3 P53_SMP=$4 sh "$top/tools/p53/boot.sh" "$1" bios 600 "$2"
    echo "boot $(basename "$2"): status $?"
    grep -a "^mmbench\|root task" "$2"
}
for s in 1 2; do for i in 1 2 3; do
    boot build/bench-mm/joist.iso "$out/p53-kvm-smp$s-$i.log" 256 $s
done; done
for i in 1 2 3; do
    boot build/bench-mm-big/joist.iso "$out/p53-kvm-smp1-big-$i.log" 1024 1
done
