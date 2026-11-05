#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Mutation check: apply each mutation in test/mutations.txt to its own scratch
# copy of the tree (model/, the kernel's core and mm's library, which it tests), build, run
# the whole test suite, and require a failure (property or unit test). The
# working tree is never modified.
# usage: mutate.sh CC
# MUTATE_ONLY=regex runs only the mutations whose names match (extended regex).
# MUTATE_JOBS=n runs n mutations at once (default: the number of CPUs); the
# results are printed in list order whatever the order they finish in.
# MUTATE_TIMEOUT=s bounds each mutant's test run (default 3600): one that runs
# longer is reported as TIMEOUT and fails the check, as a survivor does; a
# hang is not a catch, since it may be a test that never reaches its check.
set -u
if [ "${1:-}" = "--one" ]; then
    # Worker: mutation number $2 of the list in $MUT_WORK/list.
    n=$2
    w="$MUT_WORK/m$n"
    line=$(sed -n "${n}p" "$MUT_WORK/list")
    name=$(printf '%s' "$line" | awk -F '\037' '{print $1}' | sed 's/[[:space:]]*$//')
    file=$(printf '%s' "$line" | awk -F '\037' '{print $2}' | tr -d ' ')
    from=$(printf '%s' "$line" | awk -F '\037' '{print $3}' | sed 's/^ //; s/ $//')
    to=$(printf '%s' "$line" | awk -F '\037' '{print $4}' | sed 's/^ //; s/ $//')
    res="$MUT_WORK/res/$(printf '%04d' "$n")"
    mkdir -p "$w/kernel"
    cp -r "$MUT_HERE" "$w/model"
    rm -rf "$w/model/build"
    cp -r "$MUT_HERE/../kernel/core" "$MUT_HERE/../kernel/include" "$w/kernel/"
    mkdir -p "$w/servers"
    cp -r "$MUT_HERE/../servers/mm" "$w/servers/" # mm's library (decision 0023)
    mkdir -p "$w/glibc/sysdeps"
    cp -r "$MUT_HERE/../glibc/sysdeps/joist" "$w/glibc/sysdeps/" # glibc's port names (Phase 5 step 7)
    if ! python3 - "$w/model/$file" "$from" "$to" <<'PY'
import sys
p, a, b = sys.argv[1:]
s = open(p).read()
if a not in s:
    sys.exit(1)
open(p, 'w').write(s.replace(a, b, 1))
PY
    then
        echo "STALE  $name: text not found in $file" > "$res"
    elif ! make -s -C "$w/model" CC="$MUT_CC" build/plain/run-tests > "$w/build.log" 2>&1; then
        echo "STALE  $name: mutant does not build" > "$res"
    elif timeout "${MUTATE_TIMEOUT:-3600}" "$w/model/build/plain/run-tests" > "$w/out" 2>&1; then
        echo "SURVIVED $name" > "$res"
    elif [ $? -eq 124 ]; then
        echo "TIMEOUT $name: the tests ran over ${MUTATE_TIMEOUT:-3600} s" > "$res"
    else
        echo "caught $name: $( (grep -m1 'seed .* step' "$w/out" || grep -m1 'failed:' "$w/out") | sed 's/^ *//' | cut -c1-110)" > "$res"
    fi
    rm -rf "$w"
    exit 0
fi

MUT_CC=${1:-clang}
MUT_HERE=$(cd "$(dirname "$0")/.." && pwd)
MUT_WORK=$(mktemp -d)
export MUT_CC MUT_HERE MUT_WORK
trap 'rm -rf "$MUT_WORK"' EXIT
mkdir -p "$MUT_WORK/res"
# The selected mutations, one per line, fields separated by \037.
grep -v '^#' "$MUT_HERE/test/mutations.txt" | grep -v '^[[:space:]]*$' | sed 's/ @@ /\x1f/g' |
    while IFS= read -r line; do
        name=$(printf '%s' "$line" | awk -F '\037' '{print $1}' | sed 's/[[:space:]]*$//')
        if [ -n "${MUTATE_ONLY:-}" ] && ! echo "$name" | grep -Eq "$MUTATE_ONLY"; then continue; fi
        printf '%s\n' "$line"
    done > "$MUT_WORK/list"
count=$(wc -l < "$MUT_WORK/list")
jobs=${MUTATE_JOBS:-$(nproc 2>/dev/null || echo 2)}
[ "$count" -gt 0 ] && seq 1 "$count" | xargs -P "$jobs" -I{} sh "$0" --one {}
fail=0
# With MUTATE_ONLY, the mutations not run are still checked for text that is
# no longer there (a part's own run would otherwise miss what it made stale:
# step 10's full run found one).
if [ -n "${MUTATE_ONLY:-}" ]; then
    grep -v '^#' "$MUT_HERE/test/mutations.txt" | grep -v '^[[:space:]]*$' | sed 's/ @@ /\x1f/g' |
        while IFS= read -r line; do
            name=$(printf '%s' "$line" | awk -F '\037' '{print $1}' | sed 's/[[:space:]]*$//')
            file=$(printf '%s' "$line" | awk -F '\037' '{print $2}' | tr -d ' ')
            from=$(printf '%s' "$line" | awk -F '\037' '{print $3}' | sed 's/^ //; s/ $//')
            python3 -c 'import sys; sys.exit(0 if sys.argv[2] in open(sys.argv[1]).read() else 1)' \
                "$MUT_HERE/$file" "$from" || echo "STALE  $name: text not found in $file"
        done > "$MUT_WORK/stale"
    cat "$MUT_WORK/stale"
    [ -s "$MUT_WORK/stale" ] && fail=1
fi
for r in "$MUT_WORK"/res/*; do
    [ -f "$r" ] || continue
    cat "$r"
    grep -q '^caught ' "$r" || fail=1
done
[ "$(ls "$MUT_WORK/res" | wc -l)" -eq "$count" ] || { echo "mutate: only $(ls "$MUT_WORK/res" | wc -l) of $count mutations reported"; fail=1; }
exit $fail
