#!/bin/bash
# Phase 0 clang inventory for the Hurd: compile every library and program
# directory with clang, independently, so a failing library (nested functions
# in libshouldbeinlibc) cannot hide the rest of the tree's compile errors.
# The Hurd does not build out-of-tree: work on an in-tree copy. Never pass
# CFLAGS= on the make command line (Makeconf uses CFLAGS +=).
# usage: spike-hurd-clang.sh CC     -> ~/spike/hurd-$CC/, logs in logs/ (CC=gcc gives the baseline)
cc=${1:-clang-21}
src=~/spike/hurd-0.9.git20260919; t=~/spike/hurd-$cc
rm -rf "$t"; cp -a "$src" "$t"; cd "$t" || exit 1
rm -rf build-clang build-gcc .pc/applied-patches.bak
autoreconf -fi > autoreconf.log 2>&1 || { echo "autoreconf failed"; exit 1; }
CC=$cc ./configure --disable-profile --without-parted --without-libcrypt --without-rump \
  --without-acpica --without-libbz2 --without-libz --disable-ncursesw --without-libtirpc \
  > configure.log 2>&1; echo "configure rc=$? $(date +%F\ %T)"
dirs=$(make -s --no-print-directory --eval 'print-subdirs: ; @echo $(lib-subdirs) $(prog-subdirs)' print-subdirs)
echo "dirs: $(echo $dirs | wc -w)"
mkdir -p logs
bash ~/spike/clang-watchdog.sh 5 ~/spike/clang-watchdog-hurd-$cc.log &
for d in $dirs; do
  [ -d "$d" ] || { echo "$d: missing"; continue; }
  make -k -C "$d" > "logs/$d.log" 2>&1; rc=$?
  printf '%-22s rc=%-3s compiles=%-4s errors=%s\n' "$d" "$rc" \
    "$(grep -cE "^$cc " "logs/$d.log")" "$(grep -c 'error:' "logs/$d.log")"
done
echo "HURD_CLANG_DONE $(date +%F\ %T)"
