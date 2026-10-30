#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Decision 0002 sizing spike: build the Hurd and glibc with clang, keep going
# past errors (make -k), and record every error for classification.
cd ~/spike
export CC=clang CXX=clang++
echo "START $(date +%T)"

cd hurd-0.9.git20260919
autoreconf -fi > ../hurd-autoreconf.log 2>&1
rm -rf build-clang; mkdir build-clang; cd build-clang
../configure --disable-profile --without-parted --without-libcrypt --without-rump \
  --without-acpica --without-libbz2 --without-libz --disable-ncursesw --without-libtirpc \
  > ../../hurd-configure.log 2>&1; echo "hurd configure rc=$? $(date +%T)"
make -k -j2 > ../../hurd-make.log 2>&1; echo "hurd make rc=$? $(date +%T)"
cd ~/spike

cd glibc-2.43
rm -rf build-clang; mkdir build-clang; cd build-clang
../configure --prefix=/usr --disable-werror --disable-profile > ../../glibc-configure.log 2>&1
echo "glibc configure rc=$? $(date +%T)"
timeout 5h make -k -j2 > ../../glibc-make.log 2>&1; echo "glibc make rc=$? $(date +%T)"
echo "SPIKE_DONE $(date +%T)"
