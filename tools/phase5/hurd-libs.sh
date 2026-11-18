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
#   OUT/lib/LIB.a and OUT/include (the headers the Hurd installs, under
#   hurd/ as it does: ahead of the Debian ones glibc's build has, whose
#   ports.h is the Hurd's own), and OUT/logs/LIB.log, every compile's
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
mkdir -p "$OUT/dl" "$OUT/logs" "$OUT/lib" "$OUT/include/hurd"
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
# Of glibc's Joist runtime, only its MIG message header (joistmig's servers
# and libports use it), not the rest of sysdeps/joist.
cp "$repo/glibc/sysdeps/joist/joist_mig.h" "$OUT/include/"
INC="-nostdinc -isystem $RES/include -isystem $SR/usr/include -isystem $W/headers/include \
    -I$S/include -I$S -I$OUT/include"

# MIG's servers: joistmig, with the Hurd's flags for each (its Makefiles,
# and Makeconf's SERVERPREFIX for every server).
# (SPREFIX: the Makefile's mig-sheader-prefix, the server header's.)
mig() { # LIB DEFS PREFIX [IMACROS [SPREFIX]]
    imacros=${4:+-imacros $4}
    # shellcheck disable=SC2086
    clang -E -x c -nostdinc -I"$W/headers/include" -I"$SR/usr/include" -I"$S/$1" $imacros \
        -D__x86_64__ -DSERVERPREFIX=S_ "$2" -o "$OUT/logs/$1-$(basename "$2" .defs).defsi"
    base=$(basename "$2" .defs)
    (cd "$S/$1" && "$MIGCOM" -prefix "$3" -server "${base}Server.c" -sheader "${5:-}${base}_S.h" \
        -user /dev/null -header /dev/null < "$OUT/logs/$1-$base.defsi")
}

# MIG's user stubs (a library's MIGSTUBS ending in User.o).
migu() { # LIB DEFS
    # shellcheck disable=SC2086
    clang -E -x c -nostdinc -I"$W/headers/include" -I"$SR/usr/include" -I"$S/$1" \
        -D__x86_64__ "$2" -o "$OUT/logs/$1-$(basename "$2" .defs).udefsi"
    base=$(basename "$2" .defs)
    (cd "$S/$1" && "$MIGCOM" -user "${base}User.c" -header "${base}_U.h" \
        -server /dev/null -sheader /dev/null < "$OUT/logs/$1-$base.udefsi")
}

build() { # LIB SRC...: OUT/lib/LIB.a from the sources that compile
    # (with LIBFLAGS, more flags for this library)
    lib=$1; shift
    log=$OUT/logs/$lib.log
    : > "$log"
    objs=""
    bad=0
    for f in "$@"; do
        o=$S/$lib/$(basename "$f" .c).o
        # shellcheck disable=SC2086
        if (cd "$S/$lib" && "$CC" $CFLAGS ${LIBFLAGS:-} $INC -I. -c "$f" -o "$o") >> "$log" 2>&1; then
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

# A Makefile variable's words (its definition to its first line without a
# continuation), with the variables it names, $(X), put in once.
makevar() ( # LIB VAR (a subshell: its variables are its own when it recurses)
    v=$(awk -v n="$2" '
        !on && $0 ~ "^" n "[ \t]*:?=" { on = 1; sub("^" n "[ \t]*:?=", "") }
        on { line = $0; c = sub(/\\$/, "", line); printf "%s ", line; if (!c) exit }' "$S/$1/Makefile")
    out=""
    for w in $v; do
        case $w in
        '$('*')') name=${w#\$(}; out="$out $(makevar "$1" "${name%)}")" ;;
        *) out="$out $w" ;;
        esac
    done
    echo "$out"
)
# The Makefile's SRCS, and Joist's own files, those of the overlay not among them.
srcs() { # LIB
    s=$(makevar "$1" SRCS)
    for f in "$repo"/hurd/joist/"$1"/*.c; do
        [ -f "$f" ] || continue
        case " $s " in *" $(basename "$f") "*) ;; *) s="$s $(basename "$f")" ;; esac
    done
    echo "$s"
}

build libihash ihash.c murmur3.c
cp "$S/libihash/ihash.h" "$OUT/include/hurd/"
# libshouldbeinlibc whole (libports needs its assertions; libiohelp,
# libfshelp and libtrivfs more): a file that doesn't compile is recorded.
# shellcheck disable=SC2046
build libshouldbeinlibc $(srcs libshouldbeinlibc)
cp "$S"/libshouldbeinlibc/*.h "$OUT/include/"

mig libports "$W/headers/include/mach/notify.defs" ports_ "$S/libports/mig-mutate.h"
mig libports "$W/headers/include/hurd/interrupt.defs" ports_ "$S/libports/mig-mutate.h"
# shellcheck disable=SC2046
LIBFLAGS=-DPORTS_JOIST_INTERNAL build libports $(srcs libports) notifyServer.c interruptServer.c
cp "$S/libports/ports.h" "$S/libports/port-deref-deferred.h" "$OUT/include/hurd/"

# Phase 5 step 10 (the owner's design of 2026-11-16): what trans/hello needs
# besides, and libtrivfs, its three servers with the trivfs_ prefix on
# their routines and server headers, and fsys_reply's user stub.
# shellcheck disable=SC2046
build libiohelp $(srcs libiohelp)
cp "$S/libiohelp/iohelp.h" "$OUT/include/hurd/"
# shellcheck disable=SC2046
build libfshelp $(srcs libfshelp)
cp "$S/libfshelp/fshelp.h" "$S/libfshelp/rlock.h" "$OUT/include/hurd/"
for d in fs io fsys; do
    mig libtrivfs "$W/headers/include/hurd/$d.defs" trivfs_ "$S/libtrivfs/mig-mutate.h" trivfs_
done
migu libtrivfs "$W/headers/include/hurd/fsys_reply.defs"
# shellcheck disable=SC2046
build libtrivfs $(srcs libtrivfs) fsServer.c ioServer.c fsysServer.c fsys_replyUser.c
cp "$S/libtrivfs/trivfs.h" "$OUT/include/hurd/"
