#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# cbmc.sh - CBMC on the futex queues (cbmc_gsync.c): a wait with a wake or
# requeue at its blocking point, and a wake or requeue alone. Unwinding
# assertions make a short loop bound a failure, not a pass.
set -u
cd "$(dirname "$0")"
mkdir -p build/cbmc
ok=0
for mode in op wait; do
    def=""
    [ "$mode" = op ] && def="-DHARNESS_OP"
    if cbmc cbmc_gsync.c -I../sysdeps/joist $def --unwind 5 --unwinding-assertions --sat-solver cadical \
        --bounds-check --pointer-check --signed-overflow-check \
        --pointer-overflow-check --div-by-zero-check > "build/cbmc/$mode.log" 2>&1; then
        echo "cbmc: $mode: VERIFICATION SUCCESSFUL"
        ok=$((ok + 1))
    else
        echo "cbmc: $mode: FAILED (build/cbmc/$mode.log)"
        grep -E "FAILURE|error" "build/cbmc/$mode.log" | head -10
    fi
done
[ "$ok" -eq 2 ]
