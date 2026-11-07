#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# glibc-tests-link.sh - glibc's own tests, linked static for Joist (Phase 5
# step 7, the design's item 11 d). glibc's `make check run-built-tests=no`
# builds them dynamic; each test (test.o, its driver from
# support/test-driver.c in it) is linked again here as glibctest is
# (glibc-test.sh): crt0.o, libsupport, libc.a, at 0x400000, stripped. A
# test glibc links with more objects of its own directory (the libm
# tests' libm-test-support-TYPE.o) gets them too, as glibc's own link
# line in WORKDIR/logs/tests-all.log names them.
#
# usage: tools/phase5/glibc-tests-link.sh WORKDIR OUT [SUBDIR...]
#   WORKDIR: glibc-cross.sh's, its tests built; OUT: a directory, given
#   OUT/SUBDIR/TEST for each test that links and OUT/link.txt, a line a
#   test: "SUBDIR/TEST linked", "SUBDIR/TEST linked, its own main" (not
#   support's test driver: the libm tests, whose options are argp's, and
#   the old tests with a main of their own), or "SUBDIR/TEST LINK-FAILED:
#   first error";
#   and OUT.debug/SUBDIR/TEST, each unstripped.
#   SUBDIR: glibc's subdirectories (default: every one with tests built).
set -eu
W=$(cd "$1" && pwd)
mkdir -p "$2"
OUT=$(cd "$2" && pwd)
shift 2
B=$W/build
L=$W/sysroot/usr/lib
[ -f "$L/libc.a" ] || { echo "no $L/libc.a: run glibc-test.sh first"; exit 1; }
RES=$(clang -print-resource-dir)
RT=$RES/lib/linux
[ -f "$RT/clang_rt.crtbegin-x86_64.o" ] || RT=$RES/lib/x86_64-unknown-linux-gnu
UNW=$(clang -print-file-name=libunwind.a)
[ -f "$UNW" ] || UNW=$(gcc -print-file-name=libgcc_eh.a 2>/dev/null || true)
SUP=$B/support/libsupport_nonshared.a
# Every directory with a test object of either name (ls of both would fail
# where one of the two matches nothing: htl, malloc, misc, ... were left out).
[ "$#" -gt 0 ] || set -- $(cd "$B" && for d in */; do d=${d%/}; for o in "$d"/tst-*.o "$d"/test-*.o; do
    [ -f "$o" ] && { echo "$d"; break; }; done; done)
# Each test's extra objects, from glibc's link lines: "SUBDIR/TEST OBJ...".
LOG=$W/logs/tests-all.log
EXTRA=$OUT/extra.txt
: > "$EXTRA"
[ -f "$LOG" ] && awk -v B="$B" '
    { o = ""; for (i = 1; i < NF; i++) if ($i == "-o") { o = $(i + 1); break } }
    o == "" || index(o, B "/") != 1 { next }
    { t = substr(o, length(B) + 2); x = ""
      for (i = 1; i <= NF; i++)
          if ($i ~ /\.o$/ && index($i, B "/") == 1 && $i != o ".o") {
              r = substr($i, length(B) + 2)
              if (r !~ /^csu\// && r ~ /^[^\/]+\/[^\/]+$/) x = x " " $i
          }
      if (x != "") print t x }' "$LOG" | sort -u > "$EXTRA"
: > "$OUT/link.txt"
n=0; ok=0
for d in "$@"; do
    mkdir -p "$OUT/$d"
    for o in "$B/$d"/tst-*.o "$B/$d"/test-*.o; do
        [ -f "$o" ] || continue
        t=$(basename "$o" .o)
        # A test glibc linked (its executable is there); not a helper object.
        [ -x "$B/$d/$t" ] || continue
        n=$((n + 1))
        more=$(awk -v t="$d/$t" '$1 == t { $1 = ""; print; exit }' "$EXTRA")
        if ld.lld -static -m elf_x86_64 -e _start --image-base=0x400000 -z max-page-size=0x1000 \
            -z norelro -L "$L" -o "$OUT/$d/$t.full" \
            "$L/crt0.o" "$L/crti.o" "$RT/clang_rt.crtbegin-x86_64.o" "$o" $more "$SUP" \
            --start-group "$L/libpthread.a" "$L"/libm-[0-9]*.a "$L/libmvec.a" "$L/libc.a" "$L/libhurduser.a" "$L/libmachuser.a" \
            "$UNW" "$RES/lib/linux/libclang_rt.builtins-x86_64.a" --end-group \
            "$RT/clang_rt.crtend-x86_64.o" "$L/crtn.o" 2> "$OUT/$d/$t.link.log"; then
            llvm-objcopy --strip-all "$OUT/$d/$t.full" "$OUT/$d/$t"
            mkdir -p "$OUT.debug/$d"
            mv "$OUT/$d/$t.full" "$OUT.debug/$d/$t" # for symbols
            rm -f "$OUT/$d/$t.link.log"
            own=
            llvm-nm "$OUT.debug/$d/$t" | grep -q ' support_test_main$' || own=", its own main"
            echo "$d/$t linked$own" >> "$OUT/link.txt"
            ok=$((ok + 1))
        else
            rm -f "$OUT/$d/$t.full"
            echo "$d/$t LINK-FAILED: $(grep -m1 'error:' "$OUT/$d/$t.link.log" | cut -c1-150)" >> "$OUT/link.txt"
        fi
    done
done
echo "linked $ok of $n tests: $OUT/link.txt"
