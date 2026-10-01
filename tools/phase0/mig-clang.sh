#!/bin/bash
# Phase 0 clang inventory: does MIG's generated C compile with clang?
# For every .defs under HURD_SRC/hurd, generate user and server stubs with mig,
# then compile each stub with clang and with gcc (the control: an error both
# report is not a clang problem). Writes mig-clang.csv to the current directory.
# usage: mig-clang.sh HURD_SRC CLANG
src=$(realpath "$1"); cc=${2:-clang}
out=$PWD/mig-clang.csv; work=$PWD/mig-work; rm -rf "$work"; mkdir -p "$work"
echo "defs,side,mig_rc,gcc_rc,gcc_errors,clang_rc,clang_errors,clang_warnings,first_clang_error" > "$out"
for d in "$src"/hurd/*.defs; do
  b=$(basename "$d" .defs)
  ( cd "$work"
    mig -I"$src" -I"$src/hurd" -I/usr/include \
        -user "${b}User.c" -server "${b}Server.c" -header "${b}.h" -sheader "${b}_S.h" \
        "$d" > "$b.mig.log" 2>&1; mrc=$?
    for side in User Server; do
      f="${b}${side}.c"
      if [ ! -s "$f" ]; then echo "$b,$side,$mrc,,,,,," >> "$out"; continue; fi
      gcc -c -O2 -Wall -I. -I"$src" -o /dev/null "$f" > "$f.gcc.log" 2>&1; grc=$?
      $cc -c -O2 -Wall -I. -I"$src" -o /dev/null "$f" > "$f.clang.log" 2>&1; crc=$?
      first=$(grep -m1 'error:' "$f.clang.log" | sed -E 's/^[^ ]*: error: //; s/,/;/g' | cut -c1-120)
      echo "$b,$side,$mrc,$grc,$(grep -c 'error:' "$f.gcc.log"),$crc,$(grep -c 'error:' "$f.clang.log"),$(grep -c 'warning:' "$f.clang.log"),$first" >> "$out"
    done )
done
echo "stubs: $(($(wc -l < "$out") - 1))"
