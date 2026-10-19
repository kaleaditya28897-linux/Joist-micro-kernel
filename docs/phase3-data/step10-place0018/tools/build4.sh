#!/bin/sh
# build4.sh : (build VM, ~/joist-ct3/kernel) the fourth batch: `pred` (per
# round and CPU, which CPU held the lock before: RUN_LOCK_PRED), and the
# earlier images again (their .text must be unchanged).
set -eu
cd ~/joist-ct3/kernel
mk() { # name kextra uextra
    make -s BUILD=build/p8-$1 KEXTRA="$2" ROOT_TASK=benchroot MODULES=ipcbench UEXTRA="$3" \
        USER_BUILD=build/p8u-$1 build/p8-$1/joist.iso >build/p8-$1.buildlog 2>&1 || { echo "$1 FAILED"; cat build/p8-$1.buildlog; exit 1; }
    echo "$1 $(sha256sum build/p8-$1/joist.iso | cut -d' ' -f1) text $(objcopy -O binary -j .text build/p8-$1/joist.elf /dev/stdout | sha256sum | cut -c1-16)"
}
mk pred "-DRUN_LOCK_DEPTH -DRUN_LOCK_PRED" "-DBENCH_ROUNDS -DBENCH_LOCK_DEPTH"
mk gate4 "" ""
mk depth4 "-DRUN_LOCK_DEPTH" "-DBENCH_ROUNDS -DBENCH_LOCK_DEPTH"
mk place4 "-DRUN_PLACE" "-DBENCH_LOCK_DEPTH"
mk order4 "-DRUN_LOCK_DEPTH -DRUN_LOCK_ORDER" "-DBENCH_ROUNDS -DBENCH_LOCK_DEPTH"
mk rep "" "-DBENCH_REPEAT=3u"
mk rep-place "-DRUN_PLACE" "-DBENCH_LOCK_DEPTH -DBENCH_REPEAT=3u"
