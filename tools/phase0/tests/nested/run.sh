#!/bin/bash
# Unit tests for tools/phase0/nested.py: each *.c must produce exactly
# *.expected.tsv (line, function). Exit status 1 on any mismatch.
cd "$(dirname "$0")" || exit 1
rc=0
for c in *.c; do
  got=$(python3 ../../nested.py "$c" 2>&1 | cut -f2-)
  [ -d "$c" ] && continue
  want=$(cat "${c%.c}.expected.tsv")
  if [ "$got" == "$want" ]; then echo "PASS $c"; else echo "FAIL $c"; diff <(echo "$want") <(echo "$got"); rc=1; fi
done
exit $rc
