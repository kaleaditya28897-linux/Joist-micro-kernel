#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# compare.sh - are two installed stages the same bytes? (decision 0026,
# layer 3: stage 2T against stage 2D, or a stage against a rebuild of it)
#
# Compares the two trees' lists of files, symlinks' targets and every
# regular file byte for byte; nothing is ignored. Prints the sha256 of each
# file of A, and says whether B matches. On any difference, diffoscope (if
# installed) writes its report of each differing file under REPORTS.
#
# usage: tools/ddc/compare.sh A B [REPORTS]
# Exits 0 if A and B are identical, 1 if not.
set -u
a=$(cd "$1" && pwd) b=$(cd "$2" && pwd)
reports=${3:-}
LC_ALL=C
export LC_ALL

list() { # DIR: every entry, with its type, and a symlink's target
    (cd "$1" && find . | sort | while IFS= read -r f; do
        if [ -L "$f" ]; then echo "l $f -> $(readlink "$f")"
        elif [ -d "$f" ]; then echo "d $f"
        else echo "f $f"; fi
    done)
}

fail=0
if ! list "$a" > "${TMPDIR:-/tmp}/compare-a.$$" ||
    ! list "$b" > "${TMPDIR:-/tmp}/compare-b.$$"; then
    echo "compare: cannot list the trees"; exit 1
fi
if ! cmp -s "${TMPDIR:-/tmp}/compare-a.$$" "${TMPDIR:-/tmp}/compare-b.$$"; then
    echo "compare: the trees hold different files:"
    diff "${TMPDIR:-/tmp}/compare-a.$$" "${TMPDIR:-/tmp}/compare-b.$$"
    fail=1
fi
rm -f "${TMPDIR:-/tmp}/compare-a.$$" "${TMPDIR:-/tmp}/compare-b.$$"

n=0 same=0
for f in $(cd "$a" && find . -type f | sort); do
    n=$((n + 1))
    sum=$(sha256sum < "$a/$f" | cut -d' ' -f1)
    if [ ! -f "$b/$f" ]; then
        echo "$sum  $f  (missing in B)"; fail=1
    elif cmp -s "$a/$f" "$b/$f"; then
        echo "$sum  $f"; same=$((same + 1))
    else
        echo "$sum  $f  DIFFERS"; fail=1
        if [ -n "$reports" ] && command -v diffoscope >/dev/null; then
            mkdir -p "$reports"
            diffoscope --text "$reports/$(echo "$f" | sed 's|^\./||; s|/|_|g').txt" \
                "$a/$f" "$b/$f" >/dev/null 2>&1
        fi
    fi
done
echo "compare: $same of $n files identical"
exit $fail
