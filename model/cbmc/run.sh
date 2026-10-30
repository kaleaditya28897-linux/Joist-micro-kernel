#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# CBMC preservation proofs, one per operation (cbmc/harness_cspace.c, mode
# JOIST_CBMC_INDUCTIVE): from any state joist_check_invariants accepts, one
# operation with arbitrary arguments keeps every invariant, at the table sizes
# below. Unwinding assertions make an insufficient loop bound a failure.
# The page tables are their smallest (one top-level entry, nodes of two
# entries, three levels); operations 11-16 (the mapping calls) start from
# states that have them, in simulated memory of JOIST_CBMC_SIM_PAGES pages
# (6). Two loops of the map checker then need more than the default bound of
# 4, and get their own (--unwindset): the walk of a space's free table pages
# (at most the simulated pages) and of its lent frames (at most the objects).
# They are found by their source text, since CBMC numbers loops by position.
# An argument OP:TYPE splits operation OP by the type of the capability in
# its target slot (cbmc/harness_cspace.c, JOIST_CBMC_SLOT_TYPE).
# Regions (0030) are at their smallest: one record, one a task, two extents,
# one table each (two pages at these sizes), so the checks' walks over a
# region's range fit the default unwinding.
# usage: cbmc/run.sh OUTDIR OP[:TYPE]...      (prints one line per run)
# CBMC_EXTRA adds options (--verbosity 9, to see the formula's size).
out=$1; shift
here=$(cd "$(dirname "$0")/.." && pwd)
objects=3
pt_top=1
pt_bits=1
# The mapping calls' states have one space (a call changes only the space it
# maps in, and the root maps in its own through its self capability) and 6
# simulated pages: one path of three tables, built from nothing. Their
# formulas are the largest (the map check walks the tables before and after).
sim_pages=6
SIZES="-DJOIST_MAX_OBJECTS=${objects}u -DJOIST_MAX_CSPACES=@SPACES@u -DJOIST_CNODE_ROOT=2u -DJOIST_CNODE_LEAF=1u -DJOIST_MAX_LEAVES=1u -DJOIST_PT_TOP=${pt_top}u -DJOIST_PT_BITS=${pt_bits}u -DJOIST_MAX_THREADS=1u -DJOIST_MAX_CPUS=1u \
-DJOIST_MAX_BADGE_ACCTS=1u -DJOIST_MAX_DEADNAMES=1u -DJOIST_PRIOS=2u \
-DJOIST_MAX_REGIONS=1u -DJOIST_REGIONS_PER_TASK=1u -DJOIST_REGION_EXTENTS=2u -DJOIST_REGION_TABLES=1u"
# The CBMC loop id of the loop on the line of FILE holding TEXT (in $loops).
loop_id() {
    line=$(grep -n -F "$2" "$1" | cut -d: -f1)
    [ -n "$line" ] && printf '%s\n' "$loops" | awk -v l="$(basename "$1") line $line " \
        '/^Loop / { id = $2; sub(":", "", id) } index($0, l) { print id; exit }'
}
for arg in "$@"; do
    op=${arg%%:*}
    split=""
    name="op$op"
    # OP/s, OP/n: a receive with callers queued on its endpoint, or not.
    case $arg in */s) op=${op%/s}; split="-DJOIST_CBMC_SENDERS=1"; name="op$op-senders" ;;
                 */n) op=${op%/n}; split="-DJOIST_CBMC_SENDERS=0"; name="op$op-nosenders" ;; esac
    tables=""
    extra=""
    unwind=4
    unwindset=""
    sizes=$(printf '%s' "$SIZES" | sed 's/@SPACES@/2/')
    # 0025's operations (17-21) need two threads, for a loan to stand.
    case $op in 1[7-9]|2[01])
        sizes=$(printf '%s' "$sizes" | sed 's/-DJOIST_MAX_THREADS=1u/-DJOIST_MAX_THREADS=2u/') ;;
    esac
    # Receiving, waiting and the timer's wake-ups (19-21) exhaust 12 GB with
    # two spaces: one space, one capability a message, and no IPC buffers.
    case $op in 19|2[01])
        sizes=$(printf '%s' "$sizes" | sed 's/-DJOIST_MAX_CSPACES=2u/-DJOIST_MAX_CSPACES=1u/')
        sizes="$sizes -DJOIST_MSG_CAPS=1u"
        extra="-DJOIST_CBMC_NO_BUFFERS" ;;
    esac
    # The mapping calls (11-16) and the region calls (22-26, decision 0030).
    case $op in 1[1-6]|2[2-6])
        tables="-DJOIST_CBMC_TABLES -DJOIST_CBMC_SIM_PAGES=${sim_pages}u"
        sizes=$(printf '%s' "$SIZES" | sed 's/@SPACES@/1/') ;;
    esac
    case $arg in *:*) split="-DJOIST_CBMC_SLOT_TYPE=${arg#*:}"; name="op$op-type${arg#*:}" ;; esac
    start=$(date +%s)
    if [ -n "$tables" ]; then
        loops=$(cbmc -I"$here/../kernel/include" -I"$here/../kernel/core" $sizes -DJOIST_CBMC -DJOIST_CBMC_INDUCTIVE \
            -DJOIST_CBMC_STEPS=1 -DJOIST_CBMC_OP="$op" $tables "$here/cbmc/harness_cspace.c" \
            "$here"/../kernel/core/*.c --show-loops 2>&1)
        map="$here/../kernel/core/map.c"
        harness="$here/cbmc/harness_cspace.c"
        free=$(loop_id "$map" "p = st->pt_free; p != 0")
        lent=$(loop_id "$map" "f = st->lent_first; f != 0")
        hfree=$(loop_id "$harness" "cbmc: free walk")
        if [ -z "$free" ] || [ -z "$lent" ] || [ -z "$hfree" ]; then
            echo "$name: the page tables' loops not found (cbmc/run.sh)"
            continue
        fi
        unwindset="--unwindset $free:$((sim_pages + 2)),$lent:$((objects + 2))"
        unwindset="$unwindset,$hfree:$((sim_pages + 2))"
    fi
    cbmc -I"$here/../kernel/include" -I"$here/../kernel/core" $sizes -DJOIST_CBMC -DJOIST_CBMC_INDUCTIVE -DJOIST_CBMC_STEPS=1 \
        -DJOIST_CBMC_OP="$op" $tables $extra $split "$here/cbmc/harness_cspace.c" "$here"/../kernel/core/*.c \
        --unwind $unwind $unwindset --unwinding-assertions --bounds-check --pointer-check \
        --signed-overflow-check --undefined-shift-check --trace ${CBMC_EXTRA:-} > "$out/$name.log" 2>&1
    echo "$name: $(( $(date +%s) - start ))s $(grep -E '^VERIFICATION|failed \(' "$out/$name.log" | tr '\n' ' ')"
done
