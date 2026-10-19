#!/bin/sh
# build2.sh : (build VM, ~/joist-ct3/kernel) the second batch: `place` (the
# placement log alone, RUN_PLACE: no counting in lock()), and gate and depth
# again from the edited run.c (their .text must be unchanged).
# Every image is the bench-gate entry (ROOT_TASK=benchroot MODULES=ipcbench,
# default BENCH_CALLS) with the KEXTRA/UEXTRA below; one build dir each.
set -eu
cd ~/joist-ct3/kernel
mk() { # name kextra uextra
    make -s BUILD=build/p8-$1 KEXTRA="$2" ROOT_TASK=benchroot MODULES=ipcbench UEXTRA="$3" \
        USER_BUILD=build/p8u-$1 build/p8-$1/joist.iso >build/p8-$1.buildlog 2>&1 || { echo "$1 FAILED"; cat build/p8-$1.buildlog; exit 1; }
    echo "$1 $(sha256sum build/p8-$1/joist.iso | cut -d' ' -f1) text $(objcopy -O binary -j .text build/p8-$1/joist.elf /dev/stdout | sha256sum | cut -c1-16)"
}
mkdir -p build
mk place "-DRUN_PLACE" "-DBENCH_LOCK_DEPTH"
mk place-r "-DRUN_PLACE" "-DBENCH_ROUNDS -DBENCH_LOCK_DEPTH"
mk gate2 "" ""
mk depth2 "-DRUN_LOCK_DEPTH" "-DBENCH_ROUNDS -DBENCH_LOCK_DEPTH"
mk order "-DRUN_LOCK_DEPTH -DRUN_LOCK_ORDER" "-DBENCH_ROUNDS -DBENCH_LOCK_DEPTH"
