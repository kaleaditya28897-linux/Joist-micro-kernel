#!/bin/sh
# io_read through decision 0032's windows, end to end (Phase 5 step 10, the
# plan's measurement): glibctest's iobench mode (user/glibctest) preads
# 2 KiB to 1 MiB of its stdin (1 MiB, module 3) from glibcroot's console,
# on the P53's own CPU, 1 and 2 CPUs, 3 boots each, as step 9's
# (docs/phase5-data/step9-copy/). Built where limine is; glibctest.elf from
# tools/phase5/glibc-test.sh (with the iobench mode). The pool as the glibc
# test runs have it (tools/phase5/glibc-tests-run.sh): a 1 MiB buffer and a
# window region need it.
# usage: docs/phase5-data/step10-ioread/bench.sh build GLIBCTEST OUT  (the build machine)
#        docs/phase5-data/step10-ioread/bench.sh boot ISO OUTDIR  (the P53)
#        (from the tree's top)
set -u
case ${1:-} in
build)
    cd kernel || exit 1
    rm -rf build/iob ../user/build/iob
    make -s BUILD=build/iob ROOT_TASK=glibcroot MODULES="mm glibctest glibcargs glibcstdin" \
        KEXTRA="-DRUN_POOL_FRAMES=2u -DRUN_POOL_PAGES=20480u" \
        UEXTRA="-DGT_POOL_PAGES=16384u -DGT_MM_TABLES=80u" \
        MMFLAGS="-DMM_MAX_TASKS=4u -DMM_TASK_PAGES=16384u -DMM_PAGES=16384u" USER_BUILD=build/iob \
        GLIBCTEST="$2" GLIBC_MODE=iobench GLIBC_STDIN_BYTES=1048576 build/iob/joist.iso ||
        { echo BUILD FAILED; exit 1; }
    cp build/iob/joist.iso "$3" ;;
boot)
    iso=$2 out=$3; mkdir -p "$out"
    for s in 1 2; do for i in 1 2 3; do
        log=$out/p53-kvm-smp$s-$i.log
        P53_MEM=256 P53_SMP=$s sh tools/p53/boot.sh "$iso" bios 300 "$log"
        echo "boot $(basename "$log"): status $?"
        grep -a "^iobench:" "$log"
    done; done ;;
*) echo "usage: bench.sh build GLIBCTEST OUT | boot ISO OUTDIR" >&2; exit 2 ;;
esac
