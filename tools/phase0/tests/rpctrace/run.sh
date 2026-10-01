#!/bin/bash
# Regression test for tools/phase0/rpctrace-gsync.patch: four threads contend
# for one lock, which forces gsync_wait/gsync_wake. The stock rpctrace
# deadlocks (killed at the cap); the patched one must finish with
# "count 8000" and equal numbers of traced waits and wakes.
# usage: run.sh RPCTRACE [RUNS]   e.g. run.sh "path/to/rpctrace -I /usr/share/msgids" 10
r=$1; n=${2:-10}; ok=0; d=$(mktemp -d)
for i in $(seq 1 "$n"); do
  timeout 60 $r -s 16 -o "$d/t$i" python3 "$(dirname "$0")/gsync-lock.py" > "$d/o$i" 2>&1; rc=$?
  w=$(grep -c 'gsync_wait (' "$d/t$i"); k=$(grep -c 'gsync_wake (' "$d/t$i")
  c=$(grep -o 'count [0-9]*' "$d/o$i")
  echo "run $i rc=$rc $c waits=$w wakes=$k"
  [ "$rc" = 0 ] && [ "$c" = "count 8000" ] && [ "$w" = "$k" ] && [ "$w" -gt 0 ] && ok=$((ok+1))
done
echo "OK $ok/$n"; rm -rf "$d"; [ "$ok" = "$n" ]
