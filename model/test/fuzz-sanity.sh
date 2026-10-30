#!/bin/sh
# Fuzzer sanity: inject bugs from test/mutations.txt into a scratch copy, build
# the fuzzer, and require it to crash within a time limit, so a fuzzer that
# reaches nothing can never look like a clean run. The working tree is never
# modified. Each bug is tried twice: from the seed corpus, which must crash
# (pass/fail), and from an empty corpus for BLIND seconds, reported only.
# usage: fuzz-sanity.sh CC SECONDS 'NAME|NAME|...' BLIND
CC=${1:-clang}; secs=${2:-120}; list=$3; blind=${4:-120}
here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0
oldifs=$IFS; IFS='|'; set -- $list; IFS=$oldifs
for want in "$@"; do
    line=$(grep -v '^#' "$here/test/mutations.txt" | sed 's/ @@ /\x1f/g' |
        awk -F"$(printf '\037')" -v w="$want" '{n=$1; sub(/[ \t]+$/, "", n)} n == w')
    [ -n "$line" ] || { echo "UNKNOWN  $want"; fail=1; continue; }
    file=$(echo "$line" | cut -d"$(printf '\037')" -f2 | tr -d ' ')
    from=$(echo "$line" | cut -d"$(printf '\037')" -f3 | sed 's/^ //; s/ $//')
    to=$(echo "$line" | cut -d"$(printf '\037')" -f4 | sed 's/^ //; s/ $//')
    rm -rf "$work/w"; mkdir -p "$work/w/model" "$work/w/kernel"; ln -sfn "$work/w/model" "$work/t"
    (cd "$here" && tar --exclude=./build -cf - .) | (cd "$work/w/model" && tar -xf -)
    cp -r "$here/../kernel/core" "$here/../kernel/include" "$work/w/kernel/"
    mkdir -p "$work/w/servers"
    cp -r "$here/../servers/mm" "$work/w/servers/" # mm's library (decision 0023), as mutate.sh
    python3 - "$work/t/$file" "$from" "$to" <<'PY' || { echo "STALE    $want"; fail=1; continue; }
import sys
p, a, b = sys.argv[1:]
s = open(p).read()
if a not in s:
    sys.exit(1)
open(p, 'w').write(s.replace(a, b, 1))
PY
    if ! make -s -C "$work/t" CC="$CC" build/fuzz/fuzz-model > "$work/build.log" 2>&1; then
        echo "STALE    $want: mutant does not build"; fail=1; continue
    fi
    # Seeds come from the unmutated tree: a mutant's own run of the executor
    # would stop at the bug, and the point is for the fuzzer to find it.
    make -s -C "$here" CC="$CC" build/fuzz-seeds/stamp > "$work/seeds.log" 2>&1 ||
        { echo "STALE    $want: seed corpus failed"; fail=1; continue; }
    # 1. From the seed corpus (pass/fail): the fuzz binary and its checks
    # reach at least what the property test reaches.
    mkdir -p "$work/t/corpus"
    start=$(date +%s)
    if "$work/t/build/fuzz/fuzz-model" -max_total_time="$secs" -max_len=8192 -len_control=0 \
        -artifact_prefix="$work/t/" "$work/t/corpus" "$here/build/fuzz-seeds" > "$work/out" 2>&1; then
        echo "MISSED   $want: no crash in ${secs}s"; fail=1
    else
        echo "caught   $want in $(( $(date +%s) - start ))s: $(grep -m1 '^fuzz_model:' "$work/out" | cut -c1-100)"
    fi
    # 2. From nothing (informational): does coverage-guided search alone
    # find it? Not a failure if not; the per-area property tests and the
    # seeded run are the guarantee.
    rm -rf "$work/t/corpus0"; mkdir -p "$work/t/corpus0"
    start=$(date +%s)
    if "$work/t/build/fuzz/fuzz-model" -max_total_time="$blind" -max_len=8192 -len_control=0 \
        -artifact_prefix="$work/t/" "$work/t/corpus0" > "$work/out0" 2>&1; then
        echo "         (from nothing: not found in ${blind}s)"
    else
        echo "         (from nothing: found in $(( $(date +%s) - start ))s)"
    fi
done
exit $fail
