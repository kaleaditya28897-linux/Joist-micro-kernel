#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# boot.sh - boot a Joist ISO on the P53's own CPU (docs/testbed.md), through
# KVM, and give QEMU's exit status: (code << 1) | 1 for the kernel's finish
# code (isa-debug-exit), 124 on timeout. What VM 112 on pvehome was until
# 2026-10-14, without Proxmox in between: QEMU's status is the real one.
#
# usage: tools/p53/boot.sh ISO FIRMWARE TIMEOUT LOG
#   FIRMWARE  bios (SeaBIOS) or uefi (OVMF)
#   TIMEOUT   seconds before QEMU is stopped
#   LOG       where to write the serial log
# Environment: P53_HOST (default aditya@<tailnet-address>), P53_SMP (default 4),
# P53_MEM (default 256: the guest's RAM in MiB), P53_CPUS (default
# "2 3 4 5": the host CPUs the virtual ones are pinned to, one each, in
# order: four cores whose other threads, 8 to 11, the build VM is moved off
# for the boot, below), P53_FIXED_RAM (default 1; 0 for fresh host pages).
#
# The guest's RAM is a file in /dev/shm kept from boot to boot, zeroed in
# place before each one: the same host pages every time, clean every time.
# Where the host put the guest's RAM decided, for a whole boot, what moving
# the kernel lock and its data between cores cost: the contended benchmark
# line varied 7.9 % from boot to boot on fresh pages, 0.7 % on the same
# ones (docs/phase3-data/README.md, *Step 10: the contended line*). The file
# goes when the P53 restarts (tmpfs); the first boot after makes it again.
set -u
iso=$1 fw=$2 limit=$3 log=$4
host=${P53_HOST:-aditya@<tailnet-address>}
smp=${P53_SMP:-4}
cpus=${P53_CPUS:-2 3 4 5}
mem=${P53_MEM:-256}
ram=""
# (2 MiB hugepages were tried instead: no better, 2026-10-15.)
[ "${P53_FIXED_RAM:-1}" = 1 ] && ram="-object memory-backend-file,id=ram0,mem-path=/dev/shm/joist-test-ram,size=${mem}M,share=on,prealloc=on -machine memory-backend=ram0"
case $fw in
bios) firmware="" ;;
uefi) firmware="-drive if=pflash,format=raw,readonly=on,file=/usr/share/qemu/edk2-x86_64-code.fd -drive if=pflash,format=raw,file=vars.fd" ;;
*) echo "boot.sh: firmware bios or uefi" >&2; exit 2 ;;
esac
ssh_() { ssh -o BatchMode=yes -o ControlMaster=auto -o ControlPath="$HOME/.ssh/cm-joist-%r@%h" \
    -o ControlPersist=120 "$host" "$@"; }
name=joist-test-$$.iso
scp -q -o BatchMode=yes -o ControlMaster=auto -o ControlPath="$HOME/.ssh/cm-joist-%r@%h" \
    -o ControlPersist=120 "$iso" "$host:/srv/joist/test/$name" || exit 2
# One run at a time: the lock is held on the P53 for the whole run.
ssh_ "flock /srv/joist/test/lock sh -s" >"$log.raw" 2>&1 <<REMOTE
cd /srv/joist/test || exit 2
rm -f serial.log
cp /usr/share/qemu/edk2-i386-vars.fd vars.fd # (OVMF's variables: fresh for every run)
if [ "${P53_FIXED_RAM:-1}" = 1 ]; then
    # The same pages (written in place, never truncated), zeroed.
    [ -f /dev/shm/joist-test-ram ] || : > /dev/shm/joist-test-ram
    dd if=/dev/zero of=/dev/shm/joist-test-ram bs=1M count=$mem conv=notrunc status=none || exit 2
fi
# The build VM (tools/p53/joist-dev2.sh: on 0, 1, 6-11) off the benchmark
# cores' second threads for the boot, and back after, whatever the boot's
# end: during a boot nothing of ours shares a core with a virtual CPU.
vm_pin() {
    for vm in \$(pgrep -f "qemu-system-x86_64 -name joist-dev2" || true); do
        for t in /proc/\$vm/task/*; do taskset -pc \$1 \${t##*/} >/dev/null 2>&1; done
    done
}
vm_pin 0,1,6,7
trap 'vm_pin 0,1,6-11' EXIT
# QEMU's own threads (main loop, serial, I/O) on the host's other CPUs, so
# none of them ever preempts a virtual CPU; the virtual CPUs alone are then
# moved to the benchmark CPUs, below.
timeout $limit taskset -c 0,1,6,7 qemu-system-x86_64 -name joist-test,debug-threads=on -machine q35,accel=kvm \\
    -cpu host -smp $smp -m ${mem}M $ram $firmware -cdrom $name -display none -vga std -no-reboot \\
    -serial file:serial.log -device isa-debug-exit,iobase=0xf4,iosize=0x04 &
qpid=\$!
# Each virtual CPU on a host CPU of its own, in order.
sleep 0.3
n=0
for c in $cpus; do
    for comm in /proc/\$(pgrep -P \$qpid -x qemu-system-x86 || echo \$qpid)/task/*/comm /proc/\$qpid/task/*/comm; do
        [ "\$(cat \$comm 2>/dev/null)" = "CPU \$n/KVM" ] && taskset -pc \$c \$(echo \$comm | cut -d/ -f5) >/dev/null 2>&1
    done
    n=\$((n + 1))
done
wait \$qpid
st=\$?
echo "boot.sh-status: \$st"
cat serial.log 2>/dev/null
rm -f $name
REMOTE
[ $? -eq 0 ] || { cat "$log.raw" >&2; rm -f "$log.raw"; exit 2; }
st=$(sed -n 's/^boot.sh-status: //p' "$log.raw")
sed '/^boot.sh-status: /d' "$log.raw" > "$log"
rm -f "$log.raw"
# QEMU's own failure to start is status 1 too, the same as a kernel that
# finished with code 0: a status from isa-debug-exit counts only with the
# kernel's last words in the log.
case ${st:-2} in
124) exit 124 ;;
*) grep -a -q "joist: finish code" "$log" || { echo "boot.sh: no finish line in the log (QEMU status ${st:-none})" >&2; exit 2; } ;;
esac
exit "$st"
