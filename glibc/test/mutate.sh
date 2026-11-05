#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# mutate.sh CC - every mutation in mutations.txt, each in its own copy of
# glibc/ (the working tree is never touched), must make the tests fail:
# built, run with a short watchdog (a lost wake-up is a hang), and caught
# by a failed check or the watchdog. At most MUTATE_JOBS (default 4) at once.
set -u
CC=${1:-clang}
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
python3 - "$HERE/test/mutations.txt" "$WORK" <<'PY'
import sys, os
src, work = sys.argv[1:]
blocks, cur, part = [], None, None
for line in open(src).read().split('\n'):
    if line.startswith('#'):
        continue
    if line.startswith('== '):
        name, f = line[3:].split()
        cur = {'name': name, 'file': f, 'find': [], 'replace': []}
        blocks.append(cur)
        part = None
    elif line in ('-- find', '-- replace'):
        part = line[3:]
    elif cur is not None and part is not None:
        cur[part].append(line)
for i, b in enumerate(blocks):
    for k in ('find', 'replace'):
        while b[k] and b[k][-1] == '':
            b[k].pop()
    d = os.path.join(work, '%03d' % i)
    os.makedirs(d)
    open(os.path.join(d, 'name'), 'w').write(b['name'])
    open(os.path.join(d, 'file'), 'w').write(b['file'])
    open(os.path.join(d, 'find'), 'w').write('\n'.join(b['find']))
    open(os.path.join(d, 'replace'), 'w').write('\n'.join(b['replace']))
PY
one() {
    d=$1
    name=$(cat "$d/name")
    cp -r "$HERE" "$d/glibc"
    rm -rf "$d/glibc/test/build"
    if ! python3 - "$d/glibc/$(cat "$d/file")" "$d/find" "$d/replace" <<'PY'
import sys
p, a, b = sys.argv[1:]
s, a, b = open(p).read(), open(a).read(), open(b).read()
if s.count(a) != 1:
    sys.exit(1)
open(p, 'w').write(s.replace(a, b))
PY
    then
        echo "STALE    $name: text not found once" > "$d/res"
    elif ! make -s -C "$d/glibc/test" CC="$CC" WARN=-w build/plain/test-gsync build/plain/test-vm > "$d/build.log" 2>&1; then
        echo "STALE    $name: does not build" > "$d/res"
    elif GSYNC_WATCHDOG=20 timeout 120 "$d/glibc/test/build/plain/test-gsync" 10 8 > "$d/out" 2>&1 &&
        timeout 120 "$d/glibc/test/build/plain/test-vm" > "$d/out" 2>&1; then
        echo "SURVIVED $name" > "$d/res"
    else
        echo "caught   $name: $(grep -m1 -E 'test-(gsync|vm): .*(fail|disagree)' "$d/out" | cut -c1-90)" > "$d/res"
    fi
    rm -rf "$d/glibc"
}
# In batches of MUTATE_JOBS (a count of `jobs -p` in a command
# substitution sees the subshell's jobs, none, and so limited nothing).
n=0
for d in "$WORK"/[0-9]*; do
    one "$d" &
    n=$((n + 1))
    if [ $((n % ${MUTATE_JOBS:-4})) -eq 0 ]; then
        wait
    fi
done
wait
bad=0
for d in "$WORK"/[0-9]*; do
    cat "$d/res"
    grep -q '^caught' "$d/res" || bad=$((bad + 1))
done
n=$(ls -d "$WORK"/[0-9]* | wc -l)
echo "mutate: $((n - bad)) of $n caught"
[ "$bad" -eq 0 ]
