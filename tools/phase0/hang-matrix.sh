#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Decision 0002: is clang's getPresumedLoc hang specific to the Hurd?
# Runs s_lround.c's exact glibc build command (docs/phase0-data/amd64/
# clang-hang-s_lround.cmd) at -fsyntax-only -w, for each clang version and for
# the Hurd target and the Linux target. A compile still running at the cap is
# stopped with SIGSEGV so LLVM prints its stack and parser position.
# usage: hang-matrix.sh CMDFILE GLIBC_SUBDIR CAP_SECONDS VERSION...
cmdfile=$1; dir=$2; cap=$3; shift 3
base=$(sed -E 's# -o [^ ]+##; s# -MD -MP -MF [^ ]+##; s/ -c / -fsyntax-only -w /' "$cmdfile")
cd "$dir" || exit 1
printf '%-9s %-22s %-5s %-6s %s\n' version target rc secs "parser position / top Sema frame (if stopped)"
for v in "$@"; do
  for target in default x86_64-linux-gnu; do
    cmd="${base/#clang /clang-$v }"
    [ "$target" != default ] && cmd="${cmd/#clang-$v /clang-$v --target=$target }"
    s=$(date +%s)
    timeout -s SEGV "$cap" bash -c "$cmd" > /tmp/hm.$v.$target.out 2>&1; rc=$?
    secs=$(( $(date +%s) - s ))
    where=""
    if [ "$secs" -ge "$cap" ]; then
      where="$(grep -m1 'current parser token\|parser at end' /tmp/hm.$v.$target.out | sed 's/^1\.\t//' | cut -c1-70) | $(grep -m1 -oE 'clang::Sema::[A-Za-z]+' /tmp/hm.$v.$target.out)"
    else
      where="finished: $(grep -c 'error:' /tmp/hm.$v.$target.out) errors"
    fi
    printf '%-9s %-22s %-5s %-6s %s\n' "$v" "$target" "$rc" "$secs" "$where"
  done
done
