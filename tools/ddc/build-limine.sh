#!/bin/sh
# build-limine.sh - Limine from its pinned, signed source, built with the
# verified LLVM (decision 0026, layer 3; docs/toolchain/record.md).
#
# Checks the toolchain against the record (check-toolchain.sh) and both
# source tarballs against their recorded hashes. Builds nasm from source
# with the verified Clang (Limine's BIOS and UEFI parts need an assembler,
# and a binary nasm would be one more compiler nobody verified). Then
# builds Limine's BIOS and UEFI x86-64 parts, its CD and PXE images and
# its host tool (`limine bios-install`) with the verified Clang, lld and
# binary tools, and that nasm. With NASM set, that nasm is used instead
# and none is built: the DDC of nasm (tools/ddc/nasm-ddc.sh) builds Limine
# with each of its nasms, and only the nasm differs. mtools (from the system) packs the UEFI CD image.
# OUT then holds what the ISO needs, with a SHA256SUMS; the kernel's
# Makefile takes it as LIMINE_DIR.
#
# usage: tools/ddc/build-limine.sh LLVM_BIN SRCDIR OUT
#   LLVM_BIN  the verified LLVM's bin directory
#   SRCDIR    holds limine-12.9.2.tar.xz and nasm-2.16.03.tar.xz
# Environment: JOBS (default 6), NASM (a nasm to use, not built here).
# Needs: make, mtools (mcopy, mformat), and the host's C library for the
# host programs (nasm, limine).
set -eu
llvm=$(cd "$1" && pwd)
srcdir=$(cd "$2" && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
top=$(cd "$(dirname "$0")/../.." && pwd)
jobs=${JOBS:-6}
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1791127963} # the signature's time, 2026-10-04 15:32:43 UTC
export SOURCE_DATE_EPOCH
LC_ALL=C TZ=UTC
export LC_ALL TZ
umask 022

"$top/tools/ddc/check-toolchain.sh" "$llvm/.."
cd "$srcdir"
sha256sum -c <<EOF
9d104aa70b7cf017899b1f79426f55f78206372daccb059a93732febba59ad37  limine-12.9.2.tar.xz
1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148  nasm-2.16.03.tar.xz
EOF

# Always the same directory: Limine always asks nasm for DWARF, nasm writes
# its working directory into it (and into the build ID), and nasm has no
# prefix map. One build at a time.
work=/tmp/joist-build-limine
mkdir "$work" || { echo "build-limine: $work exists (another build?)"; exit 1; }
trap 'rm -rf "$work"' EXIT
# Host programs: the verified Clang, for the host (it defaults to i686
# musl), linked by the verified lld against the host's C library.
hostcc="$llvm/clang --target=x86_64-linux-gnu"
# The build directory's name never reaches what C compiles (Limine takes
# its target flags from CFLAGS).
cflags="-g -O2 -pipe -ffile-prefix-map=$work=/build"

mkdir -p "$work/bin"
if [ -n "${NASM:-}" ]; then
    cp "$NASM" "$work/bin/nasm"
else
    tar -C "$work" -xJf "$srcdir/nasm-2.16.03.tar.xz"
    (cd "$work/nasm-2.16.03" && ./configure CC="$hostcc" CFLAGS="$cflags" LDFLAGS=-fuse-ld=lld \
        AR="$llvm/llvm-ar" RANLIB="$llvm/llvm-ar s" >/dev/null && make -j"$jobs" nasm >/dev/null)
    cp "$work/nasm-2.16.03/nasm" "$work/bin/"
fi
PATH=$work/bin:$PATH
"$work/bin/nasm" -v

tar -C "$work" -xJf "$srcdir/limine-12.9.2.tar.xz"
cd "$work/limine-12.9.2"
./configure --enable-bios --enable-bios-cd --enable-bios-pxe --enable-uefi-x86-64 --enable-uefi-cd \
    CC="$hostcc" CFLAGS="$cflags" LDFLAGS=-fuse-ld=lld STRIP=true \
    CC_FOR_TARGET="$llvm/clang" LD_FOR_TARGET="$llvm/ld.lld" \
    OBJCOPY_FOR_TARGET="$llvm/llvm-objcopy" OBJDUMP_FOR_TARGET="$llvm/llvm-objdump" \
    READELF_FOR_TARGET="$llvm/llvm-readelf" >/dev/null
make -j"$jobs" >/dev/null

rm -rf "$out"
mkdir -p "$out"
cp bin/limine-bios.sys bin/limine-bios-cd.bin bin/limine-bios-pxe.bin bin/limine-uefi-cd.bin \
    bin/BOOTX64.EFI bin/limine "$out/"
cp "$work/bin/nasm" "$out/nasm"
(cd "$out" && sha256sum limine-bios.sys limine-bios-cd.bin limine-bios-pxe.bin \
    limine-uefi-cd.bin BOOTX64.EFI limine nasm > SHA256SUMS && cat SHA256SUMS)
