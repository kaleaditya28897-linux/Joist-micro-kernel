#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# glibc-tests-run.sh - glibc's own tests on Joist (Phase 5 step 7, the
# design's item 11 d): each test glibc-tests-link.sh linked, booted under
# QEMU with KVM as user/glibcroot's program, with --direct (no fork; the
# driver's own timeout needs signals, so the root's 60 s is the limit)
# unless TESTS/link.txt says it has its own main (no test driver, no
# --direct: the libm tests reject it).
# glibcroot passes its stdout through and prints its exit status.
#
# usage: tools/phase5/glibc-tests-run.sh TESTS CONSOLE OUT [JOBS] [CPUS]
#   TESTS: glibc-tests-link.sh's OUT; CONSOLE: glibc-test.sh's
#   OUT.console.o; OUT: a directory, given OUT/results.txt (a line a
#   test: "SUBDIR/TEST RESULT", RESULT one of PASS, FAIL (its exit code),
#   UNSUPPORTED (77), TIMEOUT, NO-STATUS (it ended, or the run did, with no
#   status), and OUT/logs/SUBDIR/TEST.log, each run's serial output.
#   JOBS: runs at once (default 4); CPUS: each machine's (default 1).
#   GT_UEXTRA: more flags for the user programs (-DMM_REPORT_REFUSED).
# Run from anywhere; it builds the kernel and glibcroot here first.
set -eu
TESTS=$(cd "$1" && pwd)
CONSOLE=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
mkdir -p "$3"
OUT=$(cd "$3" && pwd)
JOBS=${4:-4}
CPUS=${5:-1}
here=$(cd "$(dirname "$0")" && pwd)
K=$(cd "$here/../../kernel" && pwd)
[ -w /dev/kvm ] || { echo "needs /dev/kvm"; exit 1; }

# The kernel, mm and glibcroot with its console, once (mm with 64 regions
# a task, as the selftest's glibc runs); any test stands in for the
# program while the ISO's tree is made.
first=$(find "$TESTS" -mindepth 2 -type f ! -name '*.log' | head -1)
# More memory than the selftest's (glibc's string tests use buffers of
# megabytes), as mm's large benchmark build (docs/phase4-data/step7-exit):
# the root's memory two frames of 80 MiB; mm's pool 64 MiB, of which the
# program's tree may have 32; its four task records' page-table runs of 96
# pages (2 + 2 per 2 MiB span the program touches), and 80 pages more for
# mm's own tables over its pool.
# The program's stack 3 MiB (glibcroot's default is 128 KiB): some of
# glibc's tests have megabytes on it (test-memset, tst-widetext).
b=build/glibc-tests
# Afresh: make doesn't see flags change.
rm -rf "$K/$b" "$K/../user/build/glibc-tests"
(cd "$K" && make -s BUILD=$b ROOT_TASK=glibcroot MODULES="mm glibctest" \
    UEXTRA="-DMM_REGIONS=64u -DGT_POOL_PAGES=16384u -DGT_MM_TABLES=80u -DGT_STACK_PAGES=768u ${GT_UEXTRA:-}" \
    MMFLAGS="-DMM_MAX_TASKS=4u -DMM_TASK_PAGES=16384u -DMM_PAGES=16384u -DMM_TABLE_PAGES=96u" \
    KEXTRA="-DRUN_POOL_FRAMES=2u -DRUN_POOL_PAGES=20480u" \
    USER_BUILD=build/glibc-tests GLIBCTEST="$first" GLIBC_CONSOLE="$CONSOLE" $b/joist.iso > "$OUT/build.log" 2>&1) ||
    { echo "build failed: $OUT/build.log"; exit 1; }
LIMINE_DIR=${LIMINE_DIR:-/usr/share/limine}
LIMINE=limine
[ -x "$LIMINE_DIR/limine" ] && LIMINE=$LIMINE_DIR/limine

one() { # SLOT SUBDIR/TEST: its result line
    w=$OUT/work/$1
    rm -rf "$w" && mkdir -p "$w"
    cp -r "$K/$b/iso" "$w/iso"
    cp "$TESTS/$2" "$w/iso/boot/glibctest.elf"
    direct=" --direct"
    grep -q "^$2 linked, its own main\$" "$TESTS/link.txt" 2>/dev/null && direct=
    printf '%s%s\n' "$(basename "$2")" "$direct" > "$w/iso/boot/glibcargs.txt"
    echo "    module_path: boot():/boot/glibcargs.txt" >> "$w/iso/boot/limine/limine.conf"
    # As kernel/Makefile's joist.iso.
    xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot \
        -boot-load-size 4 -boot-info-table -hfsplus -apm-block-size 2048 \
        --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
        --protective-msdos-label "$w/iso" -o "$w/t.iso" 2>/dev/null
    "$LIMINE" bios-install "$w/t.iso" 2>/dev/null || { echo "$2 NO-ISO"; return; }
    log=$OUT/logs/$2.log
    mkdir -p "$(dirname "$log")"
    timeout 100 qemu-system-x86_64 -M q35 -m 512M -cpu host -enable-kvm -smp "$CPUS" -cdrom "$w/t.iso" \
        -display none -no-reboot -serial file:"$log" -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        > /dev/null 2>&1 || true
    code=$(sed -n 's/^glibcroot: exit status [0-9]*, code \([0-9]*\).*/\1/p' "$log" | tr -d '\r' | head -1)
    if [ -n "$code" ]; then
        case $code in 0) r=PASS ;; 77) r=UNSUPPORTED ;; *) r="FAIL $code" ;; esac
    elif grep -a -q 'glibcroot: TIMEOUT' "$log"; then
        r=TIMEOUT
    else
        r=NO-STATUS
    fi
    echo "$2 $r"
    rm -rf "$w"
}

list=$(cd "$TESTS" && find . -mindepth 2 -type f ! -name '*.log' | sed 's#^\./##' | sort)
: > "$OUT/results.txt"
i=0
for t in $list; do
    one $((i % JOBS)) "$t" >> "$OUT/results.txt" &
    i=$((i + 1))
    [ $((i % JOBS)) -eq 0 ] && wait
done
wait
sort -o "$OUT/results.txt" "$OUT/results.txt"
echo "$(wc -l < "$OUT/results.txt") tests: $(awk '{print $2}' "$OUT/results.txt" | sort | uniq -c | tr '\n' ' ')"
