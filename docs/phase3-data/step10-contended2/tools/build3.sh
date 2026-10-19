#!/bin/sh
# build3.sh : (build VM, ~/joist-ct2/kernel) the third batch: per-batch time
# series of the contended line (BENCH_SERIES), with and without a lead time
# for the busy CPUs before the measurement (BENCH_LEAD_MS).
set -eu
cd ~/joist-ct2/kernel
mk() { # name kextra uextra
    make -s BUILD=build/c2-$1 KEXTRA="$2" ROOT_TASK=benchroot MODULES=ipcbench UEXTRA="$3" \
        USER_BUILD=build/c2u-$1 build/c2-$1/joist.iso >build/c2-$1.buildlog 2>&1 || { echo "$1 FAILED"; cat build/c2-$1.buildlog; exit 1; }
    echo "$1 $(sha256sum build/c2-$1/joist.iso | cut -d' ' -f1)"
}
T="-DBENCH_ROUNDS -DBENCH_SERIES"
mk ts "" "$T"
mk ts-lead300 "" "$T -DBENCH_LEAD_MS=300u"
mk ts-lead1000 "" "$T -DBENCH_LEAD_MS=1000u"
mk ts-b1 "" "$T -DBENCH_BUSY=1"
mk ts-s3000 "" "$T -DBENCH_BUSY_SPIN=3000"
mk ts-depth "-DRUN_LOCK_DEPTH" "$T -DBENCH_LOCK_DEPTH"
mk ts-depth-b1 "-DRUN_LOCK_DEPTH" "$T -DBENCH_LOCK_DEPTH -DBENCH_BUSY=1"
