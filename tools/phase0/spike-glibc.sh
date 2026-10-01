#!/bin/bash
# Decision 0002: build glibc with clang, keep going past errors, and keep the
# full log so every error can be classified. Out-of-tree is fine for glibc
# (unlike the Hurd). No -j: GNU Mach is uniprocessor, nproc is 1.
# usage: spike-glibc.sh [CC CXX]   (default clang clang++; gcc g++ is the baseline)
cc=${1:-clang}; cxx=${2:-clang++}; tag=$(basename "$cc")
cd ~/spike/glibc-2.43
rm -rf build-$tag; mkdir build-$tag; cd build-$tag
CC=$cc CXX=$cxx ../configure --prefix=/usr --disable-werror --disable-profile \
  > ../../glibc-$tag-configure.log 2>&1
echo "glibc configure rc=$? $(date +%F\ %T)"
make -k > ../../glibc-$tag-make.log 2>&1
echo "glibc make rc=$? $(date +%F\ %T)"
cd ~/spike
grep 'error:' glibc-$tag-make.log | sort -u > glibc-$tag-errors.txt
echo "distinct errors: $(wc -l < glibc-$tag-errors.txt)"
