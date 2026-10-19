#!/bin/sh
# build.sh : (runs on the build VM, in ~/joist-ct2/kernel) the contend2 images.
# Every image is the bench-gate entry (ROOT_TASK=benchroot MODULES=ipcbench,
# default BENCH_CALLS) with the KEXTRA/UEXTRA below; one build dir each.
set -eu
cd ~/joist-ct2/kernel
mk() { # name kextra uextra
    make -s BUILD=build/c2-$1 KEXTRA="$2" ROOT_TASK=benchroot MODULES=ipcbench UEXTRA="$3" \
        USER_BUILD=build/c2u-$1 build/c2-$1/joist.iso >build/c2-$1.buildlog 2>&1 || { echo "$1 FAILED"; cat build/c2-$1.buildlog; exit 1; }
    echo "$1 $(sha256sum build/c2-$1/joist.iso | cut -d' ' -f1)"
}
mkdir -p build
S="-DRUN_LOCK_STATS"; U="-DBENCH_LOCK_STATS"
mk gate "" ""
mk base "" "-DBENCH_ROUNDS"
mk stats "$S" "-DBENCH_ROUNDS $U"
for n in 300 1000 3000 10000; do
    mk s$n "" "-DBENCH_ROUNDS -DBENCH_BUSY_SPIN=$n"
    mk stats-s$n "$S" "-DBENCH_ROUNDS $U -DBENCH_BUSY_SPIN=$n"
done
for k in 1 2; do
    mk b$k "" "-DBENCH_ROUNDS -DBENCH_BUSY=$k"
    mk stats-b$k "$S" "-DBENCH_ROUNDS $U -DBENCH_BUSY=$k"
done
