#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Mutation check: apply each mutation in test/mutations.txt in turn to a
# scratch copy of the tree (kernel/, and user/lib, whose header a host test
# checks against the kernel's ABI), build, run the whole test suite, and require a
# failure (property or unit test). The working tree is never modified.
# usage: mutate.sh CC
CC=${1:-clang}
here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0
grep -v '^#' "$here/test/mutations.txt" | grep -v '^[[:space:]]*$' | sed 's/ @@ /\x1f/g' | while IFS="$(printf '\037')" read -r name file from to; do
    name=$(echo "$name" | sed 's/[[:space:]]*$//'); file=$(echo "$file" | tr -d ' ')
    from=$(echo "$from" | sed 's/^ //; s/ $//'); to=$(echo "$to" | sed 's/^ //; s/ $//')
    rm -rf "$work/t"; mkdir -p "$work/t/user"; cp -r "$here" "$work/t/kernel"; rm -rf "$work/t/kernel/build"
    cp -r "$here/../user/lib" "$work/t/user/"
    if ! python3 - "$work/t/kernel/$file" "$from" "$to" <<'PY'
import sys
p, a, b = sys.argv[1:]
s = open(p).read()
if a not in s:
    sys.exit(1)
open(p, 'w').write(s.replace(a, b, 1))
PY
    then echo "STALE  $name: text not found in $file"; echo 1 > "$work/failed"; continue; fi
    if ! make -s -C "$work/t/kernel" CC="$CC" build/plain/run-tests > "$work/build.log" 2>&1; then
        echo "STALE  $name: mutant does not build"; echo 1 > "$work/failed"; continue
    fi
    if "$work/t/kernel/build/plain/run-tests" > "$work/out" 2>&1; then
        echo "SURVIVED $name"; echo 1 > "$work/failed"
    else
        echo "caught $name: $( (grep -m1 'seed .* step' "$work/out" || grep -m1 'failed:' "$work/out") | sed 's/^ *//' | cut -c1-110)"
    fi
done
[ -f "$work/failed" ] && exit 1
exit 0
