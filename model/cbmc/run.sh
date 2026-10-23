#!/bin/sh
# CBMC preservation proofs, one per operation (cbmc/harness_cspace.c, mode
# JOIST_CBMC_INDUCTIVE): from any state joist_check_invariants accepts, one
# operation with arbitrary arguments keeps every invariant, at the table sizes
# below. Unwinding assertions make an insufficient loop bound a failure.
# An argument OP:TYPE splits operation OP by the type of the capability in
# its target slot (cbmc/harness_cspace.c, JOIST_CBMC_SLOT_TYPE).
# usage: cbmc/run.sh OUTDIR OP[:TYPE]...      (prints one line per run)
out=$1; shift
here=$(cd "$(dirname "$0")/.." && pwd)
SIZES="-DJOIST_MAX_OBJECTS=3u -DJOIST_MAX_CSPACES=2u -DJOIST_CNODE_ROOT=2u -DJOIST_CNODE_LEAF=1u -DJOIST_MAX_LEAVES=1u -DJOIST_PT_TOP=1u -DJOIST_MAX_THREADS=1u -DJOIST_MAX_CPUS=1u \
-DJOIST_MAX_BADGE_ACCTS=1u -DJOIST_MAX_DEADNAMES=1u -DJOIST_PRIOS=2u"
for arg in "$@"; do
    op=${arg%%:*}
    split=""
    name="op$op"
    case $arg in *:*) split="-DJOIST_CBMC_SLOT_TYPE=${arg#*:}"; name="op$op-type${arg#*:}" ;; esac
    start=$(date +%s)
    cbmc -I"$here/../kernel/include" -I"$here/../kernel/core" $SIZES -DJOIST_CBMC_INDUCTIVE -DJOIST_CBMC_STEPS=1 \
        -DJOIST_CBMC_OP="$op" $split "$here/cbmc/harness_cspace.c" "$here"/../kernel/core/*.c \
        --unwind 4 --unwinding-assertions --bounds-check --pointer-check \
        --signed-overflow-check --undefined-shift-check --trace > "$out/$name.log" 2>&1
    echo "$name: $(( $(date +%s) - start ))s $(grep -E '^VERIFICATION|failed \(' "$out/$name.log" | tr '\n' ' ')"
done
