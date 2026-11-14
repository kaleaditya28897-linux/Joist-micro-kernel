#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# libports-test.sh - libports' test server on Joist (Phase 5 step 8): its
# interface through joistmig (server and user stubs), tst-libports.c
# compiled against the cross-built glibc and hurd-libs.sh's libraries,
# linked static as glibc's tests are (glibc-tests-link.sh), and laid out
# for glibc-tests-run.sh: a copy for each seed, its argument the seed.
#
# usage: tools/phase5/libports-test.sh GLIBCWORK HURDLIBS OUT [SEEDS]
#   GLIBCWORK: glibc-cross.sh's (its sysroot installed); HURDLIBS:
#   hurd-libs.sh's OUT; OUT: a directory, given OUT/tests (TESTS for
#   glibc-tests-run.sh: libports/tst-libports-sN and link.txt) and
#   OUT/specs (its GT_SPECS: each copy's seed); SEEDS: default "1 2 3".
set -eu
W=$(cd "$1" && pwd)
H=$(cd "$2" && pwd)
mkdir -p "$3"
OUT=$(cd "$3" && pwd)
SEEDS=${4:-1 2 3}
here=$(cd "$(dirname "$0")" && pwd)
T=$here/libports-test
SR=$W/sysroot
L=$SR/usr/lib
CC=$W/bin/x86_64-gnu-clang
MIGCOM=$W/mig/libexec/x86_64-gnu-migcom
RES=$(clang -print-resource-dir)
RT=$RES/lib/linux
[ -f "$RT/clang_rt.crtbegin-x86_64.o" ] || RT=$RES/lib/x86_64-unknown-linux-gnu
UNW=$(clang -print-file-name=libunwind.a)
[ -f "$UNW" ] || UNW=$(gcc -print-file-name=libgcc_eh.a 2>/dev/null || true)

G=$OUT/gen
rm -rf "$G" "$OUT/tests" "$OUT/specs"
mkdir -p "$G" "$OUT/tests/libports" "$OUT/specs/libports"
cp "$T/lpt_types.h" "$G/"
clang -E -x c -nostdinc -I"$W/headers/include" -I"$SR/usr/include" -I"$T" \
    -D__x86_64__ -DSERVERPREFIX=S_ "$T/lpt.defs" -o "$G/lpt.defsi"
(cd "$G" && "$MIGCOM" -server lptServer.c -sheader lpt_S.h -user lptUser.c \
    -header lpt_U.h < lpt.defsi)

CFLAGS="-O2 -g -std=gnu11 -fno-stack-protector -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
    -Wall -Wno-unused-parameter -Wno-sign-compare"
# The Hurd's headers as hurd-libs.sh built them, ahead of the Debian ones.
INC="-nostdinc -I$H/include -I$G -isystem $RES/include -isystem $SR/usr/include \
    -isystem $W/headers/include"
for f in "$T/tst-libports.c" "$G/lptServer.c" "$G/lptUser.c"; do
    # shellcheck disable=SC2086
    "$CC" $CFLAGS $INC -c "$f" -o "$G/$(basename "$f" .c).o"
done
ld.lld -static -m elf_x86_64 -e _start --image-base=0x400000 -z max-page-size=0x1000 \
    -z norelro -L "$L" -o "$G/tst-libports.full" \
    "$L/crt0.o" "$L/crti.o" "$RT/clang_rt.crtbegin-x86_64.o" \
    "$G/tst-libports.o" "$G/lptServer.o" "$G/lptUser.o" \
    "$H/lib/libports.a" "$H/lib/libihash.a" "$H/lib/libshouldbeinlibc.a" \
    --start-group "$L/libpthread.a" "$L/libc.a" "$L/libhurduser.a" "$L/libmachuser.a" \
    "$UNW" "$RES/lib/linux/libclang_rt.builtins-x86_64.a" --end-group \
    "$RT/clang_rt.crtend-x86_64.o" "$L/crtn.o"
llvm-objcopy --strip-all "$G/tst-libports.full" "$G/tst-libports"
: > "$OUT/tests/link.txt"
for s in $SEEDS; do
    cp "$G/tst-libports" "$OUT/tests/libports/tst-libports-s$s"
    echo "libports/tst-libports-s$s linked, its own main" >> "$OUT/tests/link.txt"
    echo "a $s" > "$OUT/specs/libports/tst-libports-s$s.args"
done
echo "tst-libports: seeds $SEEDS in $OUT/tests"
