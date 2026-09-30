#!/bin/bash
# Decision 0002: build glibc with clang, keep going past errors, and keep the
# full log so every error can be classified. Out-of-tree is fine for glibc
# (unlike the Hurd). No -j: GNU Mach is uniprocessor, nproc is 1.
cd ~/spike/glibc-2.43
rm -rf build-clang; mkdir build-clang; cd build-clang
CC=clang CXX=clang++ ../configure --prefix=/usr --disable-werror --disable-profile \
  > ../../glibc-configure.log 2>&1
echo "glibc configure rc=$? $(date +%F\ %T)"
make -k > ../../glibc-make.log 2>&1
echo "glibc make rc=$? $(date +%F\ %T)"
cd ~/spike
grep 'error:' glibc-make.log | sort -u > clang-glibc-errors.txt
echo "distinct errors: $(wc -l < clang-glibc-errors.txt)"
