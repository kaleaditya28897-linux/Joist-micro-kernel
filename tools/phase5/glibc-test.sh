#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# glibc-test.sh - build user/glibctest with the cross-built glibc (Phase 5
# step 7, the design's item 11 d): a static x86_64-gnu ELF for
# user/glibcroot to start on Joist.
#
# usage: tools/phase5/glibc-test.sh WORKDIR OUT
#   WORKDIR: glibc-cross.sh's (its build/ is installed into WORKDIR/sysroot
#   first, if it isn't); OUT: the ELF, stripped (a boot module is at most
#   2 MiB).
set -eu
W=$(cd "$1" && pwd)
OUT=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
SR=$W/sysroot
if [ ! -f "$SR/usr/lib/libc.a" ]; then
    (cd "$W/build" && make -k install DESTDIR="$SR" > "$W/logs/install.log" 2>&1) ||
        { echo "install failed: $W/logs/install.log"; exit 1; }
fi
RES=$(clang -print-resource-dir)
RT=$RES/lib/linux
[ -f "$RT/clang_rt.crtbegin-x86_64.o" ] || RT=$RES/lib/x86_64-unknown-linux-gnu
# An unwinder for static glibc (_Unwind_*: its cancellation and cleanup
# paths): LLVM's libunwind if it is installed, else GCC's libgcc_eh, a
# stand-in (under GCC's runtime exception) until LLVM's is built here.
UNW=$(clang -print-file-name=libunwind.a)
[ -f "$UNW" ] || UNW=$(gcc -print-file-name=libgcc_eh.a 2>/dev/null || true)
[ -f "$UNW" ] || { echo "no unwinder: libunwind.a or libgcc_eh.a"; exit 1; }
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
# The Hurd's target as glibc-cross.sh builds it, its headers: glibc's
# installed ones, then Mach's and the Hurd's.
"$W/bin/x86_64-gnu-clang" -O2 -g -std=gnu11 -Wall -Wextra -Werror -fno-stack-protector \
    -nostdinc -isystem "$RES/include" -isystem "$SR/usr/include" -isystem "$W/headers/include" \
    -c "$repo/user/glibctest/glibctest.c" -o "$obj/glibctest.o"
L=$SR/usr/lib
# No RELRO: glibc makes it read-only with mprotect, and the image is the
# root's mapping, which neither libc's table nor mm knows (an image mapped
# through mm is the exec server's, later); the refusal would be fatal.
# crt0.o, a static program's start on the Hurd (gcc's spec for the gnu
# target: static-start.S, then _hurd_stack_setup and _hurd_startup).
ld.lld -static -m elf_x86_64 -e _start --image-base=0x400000 -z max-page-size=0x1000 -z norelro -L "$L" -o "$obj/glibctest.elf" \
    "$L/crt0.o" "$L/crti.o" "$RT/clang_rt.crtbegin-x86_64.o" "$obj/glibctest.o" \
    --start-group "$L/libpthread.a" "$L/libc.a" "$L/libhurduser.a" "$L/libmachuser.a" \
    "$UNW" "$RES/lib/linux/libclang_rt.builtins-x86_64.a" --end-group \
    "$RT/clang_rt.crtend-x86_64.o" "$L/crtn.o"
cp "$obj/glibctest.elf" "$OUT.debug" # for symbols: the module is the stripped one
llvm-objcopy --strip-all "$obj/glibctest.elf" "$OUT"
ls -l "$OUT"
