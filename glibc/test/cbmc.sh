#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# cbmc.sh - CBMC on the futex queues (cbmc_gsync.c): a wait with a wake or
# requeue at its blocking point, and a wake or requeue alone. Unwinding
# assertions make a short loop bound a failure, not a pass.
#
# One run per mode (op: the step alone; wait: at waiter 0's block) and
# step (none, a wake, a requeue from each word to each word): all at once
# was past CBMC (see cbmc_gsync.c). The two lock loops are bounded at one
# pass: in one thread a bucket's lock is always free and a key unchanged
# between two loads, and the unwinding assertions check that it is so.
# CBMC_JOBS=n runs n at once (default 1; each may need some GB).
set -u
cd "$(dirname "$0")"
mkdir -p build/cbmc
rm -f build/cbmc/*.log build/cbmc/*.res
jobs=${CBMC_JOBS:-1}

one() { # name, then -D flags
    name=$1
    shift
    s=$(date +%s)
    if cbmc cbmc_gsync.c -I../sysdeps/joist "$@" --unwind 5 \
        --unwindset bucket_lock.0:1,lock_own_bucket.0:1 --unwinding-assertions \
        --object-bits 14 --sat-solver cadical \
        --bounds-check --pointer-check --signed-overflow-check \
        --pointer-overflow-check --div-by-zero-check > "build/cbmc/$name.log" 2>&1; then
        r="VERIFICATION SUCCESSFUL"
    else
        r="FAILED (build/cbmc/$name.log)"
    fi
    echo "cbmc: $name: $r, $(grep -E '^\*\* [0-9]+ of [0-9]+ failed' "build/cbmc/$name.log" | sed 's/^\*\* //; s/ (.*//'), $(($(date +%s) - s)) s" > "build/cbmc/$name.res"
}

runs=""
for mode in op wait; do
    runs="$runs $mode-none:2:0:0 $mode-wake:0:0:0"
    for src in 0 1 2; do
        for dst in 0 1 2; do
            runs="$runs $mode-requeue-$src$dst:1:$src:$dst"
        done
    done
done
n=0
for r in $runs; do
    IFS=: read -r name step src dst <<END
$r
END
    def="-DHARNESS_STEP=$step -DHARNESS_SRC=$src -DHARNESS_DST=$dst"
    case $name in op-*) def="$def -DHARNESS_OP" ;; esac
    # shellcheck disable=SC2086
    one "$name" $def &
    n=$((n + 1))
    if [ $((n % jobs)) -eq 0 ]; then
        wait
    fi
done
wait
ok=0
total=0
for r in $runs; do
    name=${r%%:*}
    cat "build/cbmc/$name.res"
    total=$((total + 1))
    if grep -q "SUCCESSFUL" "build/cbmc/$name.res"; then
        ok=$((ok + 1))
    else
        grep -E "FAILURE|error" "build/cbmc/$name.log" | head -10
    fi
done
echo "cbmc: $ok of $total runs successful"
[ "$ok" -eq "$total" ]
