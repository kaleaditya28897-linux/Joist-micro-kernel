#!/bin/sh
# selftest.sh - boot test builds of the kernel that misbehave on purpose and
# check it reports each case correctly (Phase 2 step 2). Each variant is
# built with -DJOIST_SELFTEST=N in its own build directory and booted under
# BIOS; QEMU's exit status is (kernel code << 1) | 1.
# usage: test/selftest.sh   (from kernel/)
fail=0
run() { # N EXPECTED_STATUS "grep pattern"...
    n=$1; want=$2; shift 2
    b=build/selftest-$n
    make -s BUILD=$b KEXTRA=-DJOIST_SELFTEST=$n $b/joist.iso >/dev/null 2>&1 ||
        { echo "selftest $n: BUILD FAILED"; fail=1; return; }
    timeout 30 qemu-system-x86_64 -M q35 -m 256M -cdrom $b/joist.iso -display none -no-reboot \
        -serial file:$b/serial.log -device isa-debug-exit,iobase=0xf4,iosize=0x04
    st=$?
    ok=1
    [ "$st" -eq "$want" ] || ok=0
    for p in "$@"; do grep -q -- "$p" $b/serial.log || ok=0; done
    if [ $ok -eq 1 ]; then
        echo "selftest $n: ok ($(grep -m1 -E 'EXCEPTION|#BP' $b/serial.log | sed 's/^joist: //'))"
    else
        echo "selftest $n: FAILED (QEMU status $st, wanted $want)"; cat $b/serial.log; fail=1
    fi
}
run 1 5 "EXCEPTION 6 (#UD invalid opcode)" "rip 0xffffffff8"
run 2 5 "EXCEPTION 14 (#PF page fault)" "cr2 0xffff900000000000"
run 3 1 "#BP breakpoint at 0xffffffff8" "joist: ok"
exit $fail
