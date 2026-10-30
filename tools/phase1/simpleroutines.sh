#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# simpleroutines.sh - one-way Hurd RPCs (MIG simpleroutines) and whether the
# Phase 0 traces saw them (Phase 1 work item 6; 0001 open item 5).
# Run on the Hurd testbed. Output CSV: defs,simpleroutine
# usage: simpleroutines.sh > simpleroutines.csv
echo "defs,simpleroutine"
for f in /usr/include/x86_64-gnu/hurd/*.defs; do
    b=$(basename "$f" .defs)
    # MIG syntax: "simpleroutine name (" possibly after whitespace.
    grep -oE '^[[:space:]]*simpleroutine[[:space:]]+[a-z_0-9]+' "$f" |
        awk -v b="$b" '{print b "," $2}'
done
