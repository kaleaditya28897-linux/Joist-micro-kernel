#!/bin/sh
# build.sh : (runs on the build VM, in ~/joist-ct3/kernel) the place0018 images.
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
mk gate "" ""
mk base "" "-DBENCH_ROUNDS"
mk depth "-DRUN_LOCK_DEPTH" "-DBENCH_ROUNDS -DBENCH_LOCK_DEPTH"
