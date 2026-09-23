#!/bin/bash
# Build the Hurd with GCC at -O0 and dump GCC's nested-function lowering
# (-fdump-tree-nested), so capture.py can classify every nested function by
# whether it captures enclosing locals and whether its address is taken.
cd ~/spike/hurd-0.9.git20260919
rm -rf build-gcc; mkdir build-gcc; cd build-gcc
CC=gcc CFLAGS="-O0 -g0 -fdump-tree-nested" ../configure --disable-profile --without-parted \
  --without-libcrypt --without-rump --without-acpica --without-libbz2 --without-libz \
  --disable-ncursesw --without-libtirpc > ../../capture-configure.log 2>&1
echo "configure rc=$? $(date +%T)"
make -k -j2 > ../../capture-make.log 2>&1
echo "make rc=$? $(date +%T)"
echo CAPTURE_DONE
