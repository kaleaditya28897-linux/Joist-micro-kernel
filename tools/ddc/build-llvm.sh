#!/bin/sh
# build-llvm.sh - build one stage of the DDC (decision 0026, layer 3):
# Clang, lld and the binary tools from LLVM's source, with the fixed options
# of tools/ddc/llvm.cmake, by the compiler given, installed into PREFIX.
#
# usage: tools/ddc/build-llvm.sh CC CXX SRC BUILD PREFIX
#   CC, CXX  the compiler that builds this stage (a path, or a name in PATH)
#   SRC      llvm-project-21.1.8.src, unpacked
#   BUILD    the build directory; made, or emptied first
#   PREFIX   where the stage is installed
# Environment:
#   JOBS          parallel jobs (default 6)
#   LINKER        the linker, as -fuse-ld= takes it (default: the compiler's)
#   EXTRA_FLAGS   more flags for every compile and link (e.g. -m32)
#   SOURCE_DATE_EPOCH  (default 1765883392, the release's signature time)
#
# Runs on Debian (path D's stage 1') and in the bootstrapped guest (the
# others); so POSIX sh, cmake, ninja and python3 only.
set -eu
cc=$1 cxx=$2 src=$3 build=$4 prefix=$5
here=$(cd "$(dirname "$0")" && pwd)
jobs=${JOBS:-6}
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1765883392}
export SOURCE_DATE_EPOCH
# Nothing from the caller's locale, time zone or umask reaches the build.
LC_ALL=C TZ=UTC
export LC_ALL TZ
umask 022

src=$(cd "$src" && pwd)
rm -rf "$build"
mkdir -p "$build"
build=$(cd "$build" && pwd)

# The directories never reach the output, whatever they are.
flags="-ffile-prefix-map=$src=/llvm-src -ffile-prefix-map=$build=/llvm-build"
# Every stage does its floating point the same way: SSE2, no x87. LLVM's
# heuristics (the register allocator's spill weights, cost models) use
# floats, so a stage 1 built for x87 compiles differently from one built
# for SSE2. GCC's i686 default is x87 with excess precision, Clang's is
# SSE2: 2T and 2D differed until this (findings, 2026-10-30).
flags="$flags -march=pentium4 -mfpmath=sse ${EXTRA_FLAGS:-}"

set -- -G Ninja -C "$here/llvm.cmake" -S "$src/llvm" -B "$build" \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_INSTALL_PREFIX="$prefix"
[ -z "${LINKER:-}" ] || set -- "$@" -DLLVM_USE_LINKER="$LINKER"

echo "build-llvm: $cc, $src -> $prefix ($(date -u))"
cmake "$@"
ninja -C "$build" -j "$jobs" distribution
rm -rf "$prefix"
ninja -C "$build" install-distribution
echo "build-llvm: done ($(date -u))"
