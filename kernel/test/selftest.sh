#!/bin/sh
# selftest.sh - boot test builds of the kernel that misbehave on purpose and
# check it reports each case correctly (Phase 2 steps 2, 4, 6, 7 and 8). Each variant is
# built with -DJOIST_SELFTEST=N in its own build directory and booted under
# BIOS; QEMU's exit status is (kernel code << 1) | 1.
# usage: test/selftest.sh   (from kernel/)
fail=0
run() { # N EXPECTED_STATUS "grep pattern"...   (N numeric: a kernel selftest
       #  build; N a name: the normal kernel with user/N as the root task)
    n=$1; want=$2; shift 2
    b=build/selftest-$n
    ub="-fsanitize=undefined -fsanitize-trap=undefined"
    case $n in
    [0-9]*) kextra="-DJOIST_SELFTEST=$n"; root=hello ;;
    ubsan-[0-9]*) kextra="$ub -DJOIST_SELFTEST=${n#ubsan-}"; root=hello ;;
    ubsan-*) kextra="$ub"; root=${n#ubsan-} ;;
    *) kextra=""; root=$n ;;
    esac
    make -s BUILD=$b KEXTRA="$kextra" ROOT_TASK=$root $b/joist.iso >/dev/null 2>&1 ||
        { echo "selftest $n: BUILD FAILED"; fail=1; return; }
    timeout 30 qemu-system-x86_64 -M q35 -m 256M -cdrom $b/joist.iso -display none -no-reboot \
        -serial file:$b/serial.log -device isa-debug-exit,iobase=0xf4,iosize=0x04
    st=$?
    ok=1
    [ "$st" -eq "$want" ] || ok=0
    for p in "$@"; do grep -a -q -- "$p" $b/serial.log || ok=0; done
    if [ $ok -eq 1 ]; then
        echo "selftest $n: ok ($(grep -a -m1 -E 'EXCEPTION|UBSAN|#BP|root task (fault|exited|refused)|refused|sysfuzz: done' $b/serial.log | tr -d '.' | sed 's/^joist: //'))"
    else
        echo "selftest $n: FAILED (QEMU status $st, wanted $want)"; cat $b/serial.log; fail=1
    fi
}
run 1 5 "EXCEPTION 6 (#UD invalid opcode)" "rip 0xffffffff8"
run 2 5 "EXCEPTION 14 (#PF page fault)" "cr2 0xffff900000000000"
run 3 1 "#BP breakpoint at 0xffffffff8" "joist: ok"
# Step 4: the kernel's own page tables enforce W^X. Error codes: 0x3 = present
# page, write; 0x11 = present page, instruction fetch.
run 4 5 "EXCEPTION 14 (#PF page fault)" "error 0x0000000000000003"
run 5 5 "EXCEPTION 14 (#PF page fault)" "error 0x0000000000000011"
# Step 6: user mode. Root task exits with 0: status 1; root task fault: 9.
run hello 1 "hello from user mode" "root task exited with code 0"
run badptr 1 "badptr: refused as expected" "root task exited with code 0"
run ud 9 "root task fault: #UD invalid opcode at 0x0000000000400004"
run rowrite 9 "root task fault: #PF page fault at" "address 0x0000000000401000"
run spin 1 "spin: done" "root task exited with code 0"
# Step 7: the system-call fuzzer, default seed, 1 000 000 calls.
run sysfuzz 1 "sysfuzz: done" "registers preserved" "root task exited with code 0"
# The sysret hazard: a syscall in the last two bytes of the user half must be
# refused (status 7: code 3), not sysret to 0x800000000000.
run sysrethole 7 "syscall returning to non-canonical 0x0000800000000000"
# Step 8: the double fault arrives on its own stack (IST1).
run 7 5 "EXCEPTION 8 (#DF double fault)"
# Step 8: the kernel's UBSan build (trap mode): a deliberate signed overflow is
# named, and the normal root task and the fuzzer run clean under it.
run ubsan-6 5 "UBSAN: signed add overflow (kind 0)"
run ubsan-hello 1 "hello from user mode" "root task exited with code 0"
run ubsan-sysfuzz 1 "sysfuzz: done" "registers preserved"
exit $fail
