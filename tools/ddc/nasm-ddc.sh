#!/bin/sh
# nasm-ddc.sh - the diverse double-compiling of nasm (decision 0026, layer
# 3, Amendment 1; docs/toolchain/record.md), from the build VM.
#
# nasm doesn't compile itself, so it is checked through what it makes.
# Two nasms are built from the same source:
#   nasm-V  by the verified Clang, on the build VM (build-limine.sh);
#   nasm-T  by the bootstrapped GCC, in live-bootstrap's final system
#           (guest.sh), with its own make and shell; the same configure
#           options but for the compiler and its linker.
# Then Limine is built three times by build-limine.sh, the same way but
# for the nasm: nasm-V, nasm-T and the system's nasm (Debian's, a third
# witness). Every installed Limine file is compared byte for byte
# (compare.sh); nothing is ignored. V and T must be identical.
#
# usage: tools/ddc/nasm-ddc.sh LLVM_BIN SRCDIR OUT
#   LLVM_BIN  the verified LLVM's bin directory
#   SRCDIR    holds limine-12.9.2.tar.xz and nasm-2.16.03.tar.xz
#   OUT       gets nasm-v, nasm-t, limine-v, limine-t, limine-debian and
#             the comparisons' diffoscope reports
# Environment: DDC (default ~/ddc) holds lb-final.img, the bootstrap's
# final disk; JOBS (default 4) and CORES (default 4) for the builds and the
# guest. Exits 0 only if Limine-V and Limine-T are identical.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
llvm=$(cd "$1" && pwd)
srcdir=$(cd "$2" && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
ddc=${DDC:-$HOME/ddc}
JOBS=${JOBS:-4} CORES=${CORES:-4}
export JOBS CORES
PATH=$PATH:/usr/sbin:/sbin # debugfs
echo "1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148  $srcdir/nasm-2.16.03.tar.xz" |
    sha256sum -c

# Limine with nasm-V; build-limine.sh builds nasm-V itself. The nasm and
# the list of sums go beside the Limine files, so that only Limine's own
# files are compared.
limine() { # NAME: move build-limine.sh's output apart
    mv "$out/limine-$1/nasm" "$out/nasm-$1/nasm"
    rm "$out/limine-$1/SHA256SUMS"
}
rm -rf "$out/limine-v" "$out/nasm-v" "$out/limine-t" "$out/nasm-t" \
    "$out/limine-debian" "$out/nasm-debian" "$out/reports-t" "$out/reports-debian"
mkdir -p "$out/nasm-v" "$out/nasm-t" "$out/nasm-debian"
"$here/build-limine.sh" "$llvm" "$srcdir" "$out/limine-v"
limine v

# nasm-T, in the guest. Its work disk is new each time.
tmp=$(mktemp -d "${TMPDIR:-/tmp}/nasm-ddc.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/in"
cp "$srcdir/nasm-2.16.03.tar.xz" "$tmp/in/"
cat > "$tmp/in/job.sh" <<'EOF'
set -eu
export SOURCE_DATE_EPOCH=1791127963
umask 022
echo "1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148  /in/nasm-2.16.03.tar.xz" |
    sha256sum -c
gcc --version | head -1
work=/tmp/joist-build-limine
rm -rf "$work"
mkdir "$work"
tar -C "$work" -xJf /in/nasm-2.16.03.tar.xz
cd "$work/nasm-2.16.03"
# As build-limine.sh builds nasm-V, but with the bootstrapped GCC and its
# binutils; -static, since the guest's musl is not on the build VM.
./configure CC=gcc CFLAGS="-g -O2 -pipe -ffile-prefix-map=$work=/build" LDFLAGS=-static
make -j4 nasm
./nasm -v
rm -rf /work/nasm-t
mkdir /work/nasm-t
cp nasm /work/nasm-t/nasm
sha256sum /work/nasm-t/nasm
EOF
mkdir -p "$ddc/logs"
log=$ddc/logs/nasm-t-$(date -u +%Y%m%dT%H%M%S).log
echo "nasm-ddc: nasm-T in the guest, console in $log"
RAM=2048 WORK_SIZE=4G SWAP_SIZE=1G "$here/guest.sh" "$ddc/lb-final.img" "$tmp/work.img" "$tmp/in" "$log"
debugfs -R "dump /nasm-t/nasm $out/nasm-t/nasm" "$tmp/work.img" 2>/dev/null
chmod 755 "$out/nasm-t/nasm"
"$out/nasm-t/nasm" -v

NASM=$out/nasm-t/nasm "$here/build-limine.sh" "$llvm" "$srcdir" "$out/limine-t"
limine t
NASM=$(command -v nasm) "$here/build-limine.sh" "$llvm" "$srcdir" "$out/limine-debian"
limine debian

(cd "$out" && sha256sum nasm-v/nasm nasm-t/nasm nasm-debian/nasm)
st=0
echo "nasm-ddc: Limine with nasm-V against nasm-T"
"$here/compare.sh" "$out/limine-v" "$out/limine-t" "$out/reports-t" || st=1
echo "nasm-ddc: Limine with nasm-V against Debian's nasm"
"$here/compare.sh" "$out/limine-v" "$out/limine-debian" "$out/reports-debian" ||
    echo "nasm-ddc: Debian's nasm differs (a witness: reported, not a failure)"
[ "$st" = 0 ] && echo "nasm-ddc: V and T identical" || echo "nasm-ddc: V and T DIFFER"
exit $st
