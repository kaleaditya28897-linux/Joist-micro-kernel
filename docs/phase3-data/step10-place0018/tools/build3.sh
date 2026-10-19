#!/bin/sh
# build3.sh : (build VM, ~/joist-ct3/kernel) the third batch: `order` (per
# round and CPU, the gap between a CPU's consecutive tickets: RUN_LOCK_ORDER)
# and `long` (the gate image with 3x longer rounds), gate and depth again
# (their .text must be unchanged).
set -eu
cd ~/joist-ct3/kernel
mk() { # name kextra uextra
    make -s BUILD=build/p8-$1 KEXTRA="$2" ROOT_TASK=benchroot MODULES=ipcbench UEXTRA="$3" \
        USER_BUILD=build/p8u-$1 build/p8-$1/joist.iso >build/p8-$1.buildlog 2>&1 || { echo "$1 FAILED"; cat build/p8-$1.buildlog; exit 1; }
    echo "$1 $(sha256sum build/p8-$1/joist.iso | cut -d' ' -f1) text $(objcopy -O binary -j .text build/p8-$1/joist.elf /dev/stdout | sha256sum | cut -c1-16)"
}
mk order "-DRUN_LOCK_DEPTH -DRUN_LOCK_ORDER" "-DBENCH_ROUNDS -DBENCH_LOCK_DEPTH"
mk long "" "-DBENCH_ROUNDS -DBENCH_CALLS=15000u"
mk gate3 "" ""
mk depth3 "-DRUN_LOCK_DEPTH" "-DBENCH_ROUNDS -DBENCH_LOCK_DEPTH"
mk place3 "-DRUN_PLACE" "-DBENCH_LOCK_DEPTH"
