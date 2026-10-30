#!/bin/sh
# ddc.sh - run one step of the DDC of Clang and lld (decision 0026,
# layer 3) in live-bootstrap's final system, from the build VM.
#
# Puts the step's inputs on a new disk (the scripts, LLVM's source and,
# for path D, stage 1'), runs tools/ddc/stage.sh on it through guest.sh,
# and copies what the step made back out of the work disk.
#
# usage: tools/ddc/ddc.sh tools        cmake and ninja for the guest
#        tools/ddc/ddc.sh stage1T      path T's stage 1
#        tools/ddc/ddc.sh stage2 t|d [OUT]  a stage 2, copied out to
#                                      DDC/out/OUT (default NAME-stage2)
# Environment: DDC (default ~/ddc) holds lb-final.img (the bootstrap's
# final disk), work.img, llvm-src/, l3-src/ and d/stage1 (path D's
# stage 1', from build-llvm.sh on Debian); logs go to DDC/logs. KEEP_BUILD=1
# keeps a stage 2's objects on the work disk (stage.sh).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ddc=${DDC:-$HOME/ddc}
step=$1
name=${2:-}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ddc-in.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
PATH=$PATH:/usr/sbin:/sbin

cp "$here/stage.sh" "$here/build-llvm.sh" "$here/llvm.cmake" "$tmp/"
case $step in
tools)
    cp "$ddc/l3-src/cmake-3.31.6.tar.gz" "$ddc/l3-src/ninja-1.12.1.tar.gz" "$tmp/"
    echo 'exec sh /in/stage.sh tools' > "$tmp/job.sh" ;;
stage1T)
    cp "$ddc/llvm-src/llvm-project-21.1.8.src.tar.xz" "$tmp/"
    echo 'exec sh /in/stage.sh stage1T' > "$tmp/job.sh" ;;
stage2)
    cp "$ddc/llvm-src/llvm-project-21.1.8.src.tar.xz" "$tmp/"
    if [ "$name" = d ]; then
        # Stage 1' comes from Debian; it is copied in each time, so every
        # stage 2D starts from the same bytes.
        cp -a "$ddc/d/stage1" "$tmp/d-stage1"
        printf '%s\n' 'rm -rf /work/d/stage1 && mkdir -p /work/d &&' \
            'cp -a /in/d-stage1 /work/d/stage1 &&' > "$tmp/job.sh"
    fi
    echo "KEEP_BUILD=${KEEP_BUILD:-0} exec sh /in/stage.sh stage2 $name" >> "$tmp/job.sh" ;;
*)
    echo "usage: ddc.sh tools | stage1T | stage2 t|d [OUT]"; exit 2 ;;
esac

mkdir -p "$ddc/logs" "$ddc/out"
log=$ddc/logs/$step${name:+-$name}-$(date -u +%Y%m%dT%H%M%S).log
echo "ddc: $step $name, console in $log"
start=$(date +%s)
"$here/guest.sh" "$ddc/lb-final.img" "$ddc/work.img" "$tmp" "$log"
echo "ddc: $step $name took $(( $(date +%s) - start )) s"

if [ "$step" = stage2 ]; then
    out=$ddc/out/${3:-$name-stage2}
    rm -rf "$out"
    mkdir -p "$out"
    debugfs -R "rdump /$name/stage2 $out" "$ddc/work.img" 2>/dev/null
    mv "$out/stage2"/* "$out/" && rmdir "$out/stage2"
    echo "ddc: copied out to $out"
fi
