#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# hello.sh - the Hurd's trans/hello for Joist (Phase 5 step 10; the owner's
# design of 2026-11-16): its source unchanged, from hurd-libs.sh's tree,
# compiled with the cross-built glibc and linked static against
# hurd-libs.sh's libraries (libtrivfs, libfshelp, libiohelp, libports,
# libihash, libshouldbeinlibc) as glibc's tests are (glibc-tests-link.sh).
#
# usage: tools/phase5/hello.sh GLIBCWORK HURDLIBS OUT
#   GLIBCWORK: glibc-cross.sh's (its sysroot installed); HURDLIBS:
#   hurd-libs.sh's OUT; OUT: the ELF (stripped), OUT.debug beside it.
set -eu
W=$(cd "$1" && pwd)
H=$(cd "$2" && pwd)
OUT=$3
S=$H/src
SR=$W/sysroot
L=$SR/usr/lib
CC=$W/bin/x86_64-gnu-clang
RES=$(clang -print-resource-dir)
RT=$RES/lib/linux
[ -f "$RT/clang_rt.crtbegin-x86_64.o" ] || RT=$RES/lib/x86_64-unknown-linux-gnu
UNW=$(clang -print-file-name=libunwind.a)
[ -f "$UNW" ] || UNW=$(gcc -print-file-name=libgcc_eh.a 2>/dev/null || true)

# The Hurd's configure makes version.h from version.h.in; its version, the
# one configure.ac names.
grep -q '^AC_INIT(\[GNU Hurd\], \[0.9\]' "$S/configure.ac" || { echo "the Hurd's version isn't 0.9"; exit 1; }
{ echo '#define MASTER_HURD_VERSION "0.9"'; cat "$S/version.h.in"; } > "$S/version.h"

CFLAGS="-O2 -g -std=gnu11 -fno-stack-protector -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
    -Wall -Wno-unused-parameter -Wno-sign-compare"
INC="-nostdinc -I$H/include -I$S -isystem $RES/include -isystem $SR/usr/include \
    -isystem $W/headers/include"
O=$(dirname "$OUT")/hello.o
# shellcheck disable=SC2086
"$CC" $CFLAGS $INC -c "$S/trans/hello.c" -o "$O"
HL=$H/lib
ld.lld -static -m elf_x86_64 -e _start --image-base=0x400000 -z max-page-size=0x1000 \
    -z norelro -L "$L" -o "$OUT.debug" \
    "$L/crt0.o" "$L/crti.o" "$RT/clang_rt.crtbegin-x86_64.o" "$O" \
    --start-group "$HL/libtrivfs.a" "$HL/libfshelp.a" "$HL/libiohelp.a" "$HL/libports.a" \
    "$HL/libihash.a" "$HL/libshouldbeinlibc.a" \
    "$L/libpthread.a" "$L/libc.a" "$L/libhurduser.a" "$L/libmachuser.a" \
    "$UNW" "$RES/lib/linux/libclang_rt.builtins-x86_64.a" --end-group \
    "$RT/clang_rt.crtend-x86_64.o" "$L/crtn.o"
llvm-objcopy --strip-all "$OUT.debug" "$OUT"
ls -l "$OUT"

# glibcroot's side (user/glibcroot, GT_HELLO; user/Makefile's GLIBC_HELLO):
# joistmig's server for rootfs.defs (fsys_startup), and its user stubs for
# fsys.defs and io.defs, the root's calls to hello, built with glibc's
# headers on user/lib's runtime, into OUT.helloroot.o.
repo=$(cd "$(dirname "$0")/../.." && pwd)
MIGCOM=$W/mig/libexec/x86_64-gnu-migcom
G=$(dirname "$OUT")/helloroot
rm -rf "$G" && mkdir -p "$G"
defsi() { # DEFS NAME
    clang -E -x c -nostdinc -I"$W/headers/include" -I"$SR/usr/include" -D__x86_64__ \
        -DSERVERPREFIX=S_ "$1" -o "$G/$2.defsi"
}
defsi "$repo/user/glibcroot/rootfs.defs" rootfs
(cd "$G" && "$MIGCOM" -server rootfsServer.c -sheader rootfs_S.h -user /dev/null -header /dev/null \
    < rootfs.defsi)
# The node hello is attached to (what libtrivfs asks of it), prefixed node_
# beside the console's io server.
for d in rootnode rootfile; do
    defsi "$repo/user/glibcroot/$d.defs" "$d"
    (cd "$G" && "$MIGCOM" -prefix node_ -server "${d}Server.c" -sheader "${d}_S.h" -user /dev/null \
        -header /dev/null < "$d.defsi")
done
for d in fsys io; do
    defsi "$W/headers/include/hurd/$d.defs" "$d"
    # (prefixed hr_: the root's console serves io_read and io_write itself)
    (cd "$G" && "$MIGCOM" -prefix hr_ -user "${d}User.c" -header "${d}_U.h" -server /dev/null \
        -sheader /dev/null < "$d.defsi")
done
# As glibc-test.sh compiles the console's (OUT.console.o).
RFLAGS="-O2 -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -fno-pic -fno-stack-protector \
    -mgeneral-regs-only -ffreestanding"
cp "$repo/user/glibcroot/hellostat.c" "$G/"
for c in rootfsServer rootnodeServer rootfileServer fsysUser ioUser hellostat; do
    # shellcheck disable=SC2086
    "$CC" $RFLAGS -nostdinc -isystem "$RES/include" -isystem "$SR/usr/include" \
        -isystem "$W/headers/include" -I"$repo/user/lib" -I"$G" -c "$G/$c.c" -o "$G/$c.o"
done
ld.lld -r -o "$OUT.helloroot.o" "$G/rootfsServer.o" "$G/rootnodeServer.o" "$G/rootfileServer.o" \
    "$G/fsysUser.o" "$G/ioUser.o" "$G/hellostat.o"
ls -l "$OUT.helloroot.o"
