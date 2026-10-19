#!/bin/sh
# 20 boots each of two images, interleaved (a b a b ...), as tools/p53/boot.sh
# boots them (the build VM idle); then the plan's stability test of each.
# usage: campaign.sh OUTDIR A.iso B.iso
set -u
out=$1 a=$2 b=$3
repo=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$out"
: > "$out/campaign.log"
sha256sum "$a" "$b" > "$out/images.sha256"
for i in $(seq 1 20); do
    for v in a b; do
        iso=$a; [ $v = b ] && iso=$b
        "$repo/tools/p53/boot.sh" "$iso" bios 120 "$out/$v-$i.log"
        echo "$(date +%T) $v $i status $?" >> "$out/campaign.log"
    done
done
for v in a b; do
    echo "== $v"
    python3 "$repo/docs/phase3-data/step10-stays/analyze.py" "$out/$v" 20
done > "$out/analysis.txt"
cat "$out/analysis.txt"
