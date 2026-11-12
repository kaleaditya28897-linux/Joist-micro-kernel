#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# hurd-libs.sh - the Hurd's libraries for Joist (Phase 5 step 8: libports;
# step 10: libtrivfs): the Hurd's own source, fetched and checked by its
# SHA-256 as glibc's is (glibc-cross.sh), Joist's files from hurd/joist/
# laid over it (a file there replaces the Hurd's of the same name, as
# glibc/sysdeps/joist does glibc's; the owner's choice, 2026-11-12), MIG's
# servers made by joistmig, and each library compiled with the Hurd's
# target compiler against the cross-built glibc into a static archive.
#
# usage: tools/phase5/hurd-libs.sh GLIBCWORK OUT
#   GLIBCWORK: glibc-cross.sh's, its sysroot installed (glibc-test.sh);
#   OUT: a directory, given OUT/src (the Hurd's tree with the overlay),
#   OUT/lib/LIB.a and OUT/include, and OUT/logs/LIB.log, every compile's
#   output (the build goes on past a file that fails, and says which).
set -eu
W=$(cd "$1" && pwd)
mkdir -p "$2"
OUT=$(cd "$2" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
SR=$W/sysroot
[ -f "$SR/usr/lib/libc.a" ] || { echo "no $SR/usr/lib/libc.a: glibc-test.sh first"; exit 1; }
CC=$W/bin/x86_64-gnu-clang
MIGCOM=$W/mig/libexec/x86_64-gnu-migcom
RES=$(clang -print-resource-dir)
URL=https://deb.debian.org/debian/pool/main/h/hurd/hurd_0.9.git20261028.orig.tar.bz2
SUM=2a17feadadf1d1cd632c446bac259acedd1c1e1bcea786c0d2118bf9d008bd27
mkdir -p "$OUT/dl" "$OUT/logs" "$OUT/lib" "$OUT/include"
[ -f "$OUT/dl/hurd.orig.tar.bz2" ] || curl -sSf -o "$OUT/dl/hurd.orig.tar.bz2" "$URL"
echo "$SUM  $OUT/dl/hurd.orig.tar.bz2" | sha256sum -c --quiet
# The source afresh each run, then the overlay.
rm -rf "$OUT/src" && mkdir -p "$OUT/src"
tar -xjf "$OUT/dl/hurd.orig.tar.bz2" -C "$OUT/src" --strip-components=1
for d in "$repo"/hurd/joist/*/; do
    [ -d "$d" ] || continue
    lib=$(basename "$d")
    cp "$d"/* "$OUT/src/$lib/"
done
S=$OUT/src
CFLAGS="-O2 -g -std=gnu11 -fno-stack-protector -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
    -Wall -Wno-unused-parameter -Wno-sign-compare"
INC="-nostdinc -isystem $RES/include -isystem $SR/usr/include -isystem $W/headers/include \
    -I$S/include -I$S -I$repo/glibc/sysdeps/joist"

# MIG's servers: joistmig, with the Hurd's flags for each (its Makefiles).
mig() { # LIB DEFS PREFIX [IMACROS]
    imacros=${4:+-imacros $4}
    # shellcheck disable=SC2086
    clang -E -x c -nostdinc -I"$W/headers/include" -I"$SR/usr/include" -I"$S/$1" $imacros \
        -D__x86_64__ "$2" -o "$OUT/logs/$1-$(basename "$2" .defs).defsi"
    base=$(basename "$2" .defs)
    (cd "$S/$1" && "$MIGCOM" -prefix "$3" -server "${base}Server.c" -sheader "${base}_S.h" \
        -user /dev/null -header /dev/null < "$OUT/logs/$1-$base.defsi")
}

build() { # LIB SRC...: OUT/lib/LIB.a from the sources that compile
    lib=$1; shift
    log=$OUT/logs/$lib.log
    : > "$log"
    objs=""
    bad=0
    for f in "$@"; do
        o=$S/$lib/$(basename "$f" .c).o
        # shellcheck disable=SC2086
        if (cd "$S/$lib" && "$CC" $CFLAGS $INC -I. -c "$f" -o "$o") >> "$log" 2>&1; then
            objs="$objs $o"
        else
            echo "FAILED $lib/$f" >> "$log"
            bad=$((bad + 1))
        fi
    done
    # shellcheck disable=SC2086
    llvm-ar rcs "$OUT/lib/$lib.a" $objs
    echo "$lib: $(echo $objs | wc -w) objects, $bad failed (logs/$lib.log)"
}

build libihash ihash.c murmur3.c
cp "$S/libihash/ihash.h" "$OUT/include/"

mig libports "$W/headers/include/mach/notify.defs" ports_ "$S/libports/mig-mutate.h"
mig libports "$W/headers/include/hurd/interrupt.defs" ports_ "$S/libports/mig-mutate.h"
srcs=$(sed -n '/^SRCS = /,/^[^ ].*[^\\]$/p' "$S/libports/Makefile" | sed 's/^SRCS = //; s/\\//g')
# shellcheck disable=SC2086
build libports $srcs notifyServer.c interruptServer.c
cp "$S/libports/ports.h" "$S/libports/port-deref-deferred.h" "$OUT/include/"
