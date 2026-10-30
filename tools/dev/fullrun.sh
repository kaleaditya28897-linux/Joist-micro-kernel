#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# fullrun.sh - the whole verification suite, run on the build VM at the end
# of a step (docs/phase3-plan.md, *Progress*: the cadence agreed at step 3).
#
# usage (on the build VM, from the top of the tree): sh tools/dev/fullrun.sh [LOGDIR]
#   from the workstation: tools/dev/remote.sh 'sh tools/dev/fullrun.sh'
#
# Each suite's whole output goes to LOGDIR/<where>-<target>.log, and one
# line per suite is printed with make's own exit status. The last line is
# "FULL RUN: all N suites passed" or names the ones that failed, and the
# script's status says the same.
#
# (Why a script: until 2026-10-13 this was a one-line loop that piped make
# through `tail` and printed the pipe's status, which is tail's and always
# 0. A suite that didn't run at all, CBMC without its preprocessor on a new
# build VM, was reported as passing for two steps.)
set -u
logs=${1:-$HOME/fullrun-logs}
mkdir -p "$logs"
failed=""
n=0
run() { # WHERE TARGET
    n=$((n + 1))
    start=$(date +%s)
    make -C "$1" "$2" > "$logs/$1-$2.log" 2>&1
    st=$?
    echo "=== $1 $2: exit $st, $(( $(date +%s) - start ))s; $(tail -1 "$logs/$1-$2.log" | cut -c1-150)"
    [ $st -eq 0 ] || failed="$failed $1/$2"
}
for t in check mutate cbmc tla tla-mutate fuzz fuzz-sanity; do run model $t; done
for t in check mutate fuzz-abi fuzz-elf fuzz-syscall; do run kernel $t; done
echo "--- what the logs say"
grep -h -a "tests, [0-9]* failed" "$logs/model-check.log" "$logs/kernel-check.log" | sort | uniq -c
echo "model mutations caught: $(grep -a -c '^caught' "$logs/model-mutate.log"), survived or stale: $(grep -a -c '^SURVIVED\|^STALE' "$logs/model-mutate.log")"
echo "kernel mutations caught: $(grep -a -c '^caught' "$logs/kernel-mutate.log"), survived or stale: $(grep -a -c '^SURVIVED\|^STALE' "$logs/kernel-mutate.log")"
grep -h -a "^cbmc: \|^tla: \|VERIFICATION" "$logs/model-cbmc.log" "$logs/model-tla.log" | tail -9
grep -h -a "^selftest .*: \(FAILED\|SKIPPED\|BUILD\)\|^bench gate" "$logs/kernel-check.log"
echo "self-tests ok: $(grep -a -c '^selftest .*: ok' "$logs/kernel-check.log")"
if [ -z "$failed" ]; then
    echo "FULL RUN: all $n suites passed"
else
    echo "FULL RUN: FAILED:$failed"
    exit 1
fi
