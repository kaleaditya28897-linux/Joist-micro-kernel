#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Phase 0 clang inventory: build GNU Mach with clang, keep going past errors,
# keep the full log. GNU Mach builds out-of-tree (standard automake).
# usage: spike-gnumach.sh CC [MAKE_ARGS...]   (e.g. clang-21, or gcc for the baseline)
# Under clang, pass GCC_INSTALL=$(clang -print-resource-dir): GNU Mach parses
# GCC_INSTALL from the `install:` line of `$(CC) -print-search-dirs`, which
# clang does not print, so the kernel's -nostdinc build loses <stdint.h>.
cc=${1:-clang}; shift
cd ~/spike/gnumach-1.8+git20260805 || exit 1
# Always regenerate, as Debian's build does: its patches touch configure.ac,
# and the tarball's generated files come from an older automake (1.18).
autoreconf -fi > ../gnumach-autoreconf.log 2>&1 || { echo "autoreconf failed"; exit 1; }
d=build-$(basename "$cc"); rm -rf "$d"; mkdir "$d"; cd "$d"
CC=$cc ../configure --host=x86_64-gnu --enable-platform=at > ../../gnumach-$cc-configure.log 2>&1
echo "configure rc=$? $(date +%F\ %T)"
# Start the watchdog only now: it exits as soon as it sees no make running.
bash ~/spike/clang-watchdog.sh 5 ~/spike/clang-watchdog-gnumach.log &
make -k "$@" > ../../gnumach-$cc-make.log 2>&1
echo "make rc=$? $(date +%F\ %T)"
ls -la gnumach gnumach.gz 2>/dev/null
grep 'error:' ../../gnumach-$cc-make.log | sort -u | wc -l
