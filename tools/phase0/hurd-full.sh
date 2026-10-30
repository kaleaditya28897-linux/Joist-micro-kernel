#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Phase 0 baseline: a normal, top-level, in-tree build of the Hurd from a fresh
# copy (not the per-directory trees, whose dependency files from failed
# compiles reference a bare version.h and break the top-level build).
# usage: hurd-full.sh CC
cc=${1:-gcc}; t=~/spike/hurd-full-$cc
rm -rf "$t"; cp -a ~/spike/hurd-0.9.git20260919 "$t"; cd "$t" || exit 1
autoreconf -fi > autoreconf.log 2>&1 || { echo autoreconf failed; exit 1; }
CC=$cc ./configure --disable-profile --without-parted --without-libcrypt --without-rump \
  --without-acpica --without-libbz2 --without-libz --disable-ncursesw --without-libtirpc \
  > configure.log 2>&1; echo "configure rc=$? $(date +%F\ %T)"
make -k > make.log 2>&1; echo "make rc=$? $(date +%F\ %T)"
echo "errors: $(grep -c "error:" make.log)  make failures: $(grep -cE "^make.*\*\*\*" make.log)"
echo HURD_FULL_DONE
