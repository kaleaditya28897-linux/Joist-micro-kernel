#!/bin/sh
# build2.sh : (build VM, ~/joist-ct2/kernel) the second batch: RUN_LOCK_DEPTH
# builds (tickets ahead at asking, per round, no TSC) and more spin points.
set -eu
cd ~/joist-ct2/kernel
mk() { # name kextra uextra
    make -s BUILD=build/c2-$1 KEXTRA="$2" ROOT_TASK=benchroot MODULES=ipcbench UEXTRA="$3" \
        USER_BUILD=build/c2u-$1 build/c2-$1/joist.iso >build/c2-$1.buildlog 2>&1 || { echo "$1 FAILED"; cat build/c2-$1.buildlog; exit 1; }
    echo "$1 $(sha256sum build/c2-$1/joist.iso | cut -d' ' -f1)"
}
S="-DRUN_LOCK_STATS"; U="-DBENCH_LOCK_STATS"; D="-DRUN_LOCK_DEPTH"; UD="-DBENCH_LOCK_DEPTH"
for n in 100 600 2000; do mk s$n "" "-DBENCH_ROUNDS -DBENCH_BUSY_SPIN=$n"; done
for n in 600 2000; do mk stats-s$n "$S" "-DBENCH_ROUNDS $U -DBENCH_BUSY_SPIN=$n"; done
mk depth "$D" "-DBENCH_ROUNDS $UD"
for n in 100 300 600 1000 2000 3000; do mk depth-s$n "$D" "-DBENCH_ROUNDS $UD -DBENCH_BUSY_SPIN=$n"; done
for k in 1 2; do mk depth-b$k "$D" "-DBENCH_ROUNDS $UD -DBENCH_BUSY=$k"; done
# the first batch's non-depth kernels, rebuilt from the edited run.c: must be unchanged
for b in gate stats; do echo "$b-text $(objcopy -O binary -j .text build/c2-$b/joist.elf /dev/stdout | sha256sum | cut -c1-16)"; done
make -s BUILD=build/c2-recheck-gate ROOT_TASK=benchroot MODULES=ipcbench USER_BUILD=build/c2u-gate build/c2-recheck-gate/joist.iso >/dev/null 2>&1
make -s BUILD=build/c2-recheck-stats KEXTRA="$S" ROOT_TASK=benchroot MODULES=ipcbench UEXTRA="-DBENCH_ROUNDS $U" USER_BUILD=build/c2u-stats build/c2-recheck-stats/joist.iso >/dev/null 2>&1
for b in recheck-gate recheck-stats; do echo "$b-text $(objcopy -O binary -j .text build/c2-$b/joist.elf /dev/stdout | sha256sum | cut -c1-16)"; done
