#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Run the s_lround.c clang hang from a self-contained bundle (decision 0002).
# The bundle holds every file the compile opens, at its absolute path under
# ROOT. The VM's default include search list is pinned with -nostdinc so the
# host's own headers are never used, which makes the run host-independent
# except for clang itself.
# usage: hang-bundle-run.sh ROOT CMDFILE CAP_SECONDS CLANG
R=$(realpath "$1"); cmdfile=$2; cap=$3; cc=$4
sys="-nostdinc -isystem $R/usr/local/include -isystem $R/usr/lib/llvm-21/lib/clang/21/include -isystem $R/usr/include/x86_64-gnu -isystem $R/usr/include"
base=$(sed -E "s# -o [^ ]+##; s# -MD -MP -MF [^ ]+##; s/ -c / -fsyntax-only -w /; s#/home/user/spike#$R/home/user/spike#g; s#^clang #$cc $sys #" "$cmdfile")
cd "$R/home/user/spike/glibc-2.43/math" || exit 1
echo "host: $(uname -sr) | $($cc --version | head -1)"
for target in x86_64-unknown-hurd-gnu0.9 x86_64-linux-gnu; do
  s=$(date +%s)
  timeout -s SEGV "$cap" bash -c "${base/#$cc /$cc --target=$target }" > /tmp/hb.$target.out 2>&1; rc=$?
  secs=$(( $(date +%s) - s ))
  if [ "$secs" -ge "$cap" ]; then
    where="STOPPED at cap: $(grep -m1 'current parser token' /tmp/hb.$target.out | sed 's/^1\.\t//' | sed "s#$R##" | cut -c1-70) | $(grep -m1 -oE 'clang::Sema::[A-Za-z]+' /tmp/hb.$target.out)"
  else
    where="finished in ${secs}s: $(grep -c 'error:' /tmp/hb.$target.out) errors"
  fi
  printf '%-28s rc=%-4s %s\n' "$target" "$rc" "$where"
done
