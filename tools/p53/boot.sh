#!/bin/sh
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
# P53_CPUS (default "2 3 4 5": the host CPUs the virtual ones are pinned to,
# one each, in order: four cores whose other threads, 8 to 11, nothing of
# ours is pinned to).
set -u
iso=$1 fw=$2 limit=$3 log=$4
host=${P53_HOST:-aditya@<tailnet-address>}
smp=${P53_SMP:-4}
cpus=${P53_CPUS:-2 3 4 5}
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
timeout $limit qemu-system-x86_64 -name joist-test,debug-threads=on -machine q35,accel=kvm \\
    -cpu host -smp $smp -m 256M $firmware -cdrom $name -display none -vga std -no-reboot \\
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
