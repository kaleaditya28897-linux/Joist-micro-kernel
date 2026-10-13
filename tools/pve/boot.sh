#!/bin/sh
# boot.sh - boot a Joist ISO on the Proxmox test VM (docs/testbed.md, VM 112)
# and report what QEMU's exit status would have been.
#
# usage: tools/pve/boot.sh ISO FIRMWARE TIMEOUT LOG
#   FIRMWARE  bios (SeaBIOS) or uefi (OVMF)
#   TIMEOUT   seconds before the VM is stopped
#   LOG       where to write the serial log
#
# Proxmox doesn't pass on QEMU's exit status, so the status is rebuilt from
# the kernel's "joist: finish code N" line: (N << 1) | 1, as isa-debug-exit
# gives it. A VM that stopped without that line (a reset under -no-reboot,
# i.e. a triple fault) gives 0; a timeout gives 124, as timeout(1) does.
# Environment: PVE_HOST (default root@<pvehome-address>), PVE_VMID (default 112).
set -u
iso=$1 fw=$2 limit=$3 log=$4
host=${PVE_HOST:-root@<pvehome-address>}
vmid=${PVE_VMID:-112}
case $fw in bios) bios=seabios ;; uefi) bios=ovmf ;; *) echo "boot.sh: firmware bios or uefi" >&2; exit 2 ;; esac
ssh_() { ssh -o BatchMode=yes -o ControlMaster=auto -o ControlPath="$HOME/.ssh/cm-joist-%r@%h" \
    -o ControlPersist=120 "$host" "$@"; }
name=joist-test-$vmid.iso
scp -q -o BatchMode=yes -o ControlMaster=auto -o ControlPath="$HOME/.ssh/cm-joist-%r@%h" \
    -o ControlPersist=120 "$iso" "$host:/var/lib/vz/template/iso/$name" || exit 2
# One run at a time on the VM: the lock is held on pvehome for the whole run.
ssh_ "flock /root/joist-test/lock sh -s" >"$log.raw" 2>&1 <<REMOTE
set -u
qm stop $vmid --skiplock 1 >/dev/null 2>&1
rm -f /root/joist-test/serial.log
qm set $vmid --bios $bios --ide2 local:iso/$name,media=cdrom --boot order=ide2 >/dev/null || exit 2
qm start $vmid >/dev/null 2>&1 || { echo "boot.sh: qm start failed"; exit 2; }
# Each virtual CPU on a host CPU of its own, in the order of the VM's
# affinity list (Proxmox pins the VM as a whole to the set, and its virtual
# CPUs would otherwise wander within it, which shows in the timings).
pid=\$(cat /run/qemu-server/$vmid.pid 2>/dev/null)
n=0
for c in \$(qm config $vmid | sed -n 's/^affinity: //p' | tr ',' ' '); do
    for comm in /proc/\$pid/task/*/comm; do
        if [ "\$(cat \$comm 2>/dev/null)" = "CPU \$n/KVM" ]; then
            taskset -pc \$c \$(echo \$comm | cut -d/ -f5) >/dev/null 2>&1
        fi
    done
    n=\$((n + 1))
done
end=\$((\$(date +%s) + $limit)); state=timeout
while [ \$(date +%s) -lt \$end ]; do
    qm status $vmid | grep -q stopped && { state=stopped; break; }
    sleep 0.2
done
[ \$state = timeout ] && qm stop $vmid >/dev/null 2>&1
echo "boot.sh-state: \$state"
cat /root/joist-test/serial.log 2>/dev/null
REMOTE
st=$?
[ $st -eq 0 ] || { cat "$log.raw" >&2; rm -f "$log.raw"; exit 2; }
state=$(sed -n 's/^boot.sh-state: //p' "$log.raw")
sed '/^boot.sh-state: /d' "$log.raw" > "$log"
rm -f "$log.raw"
[ "$state" = timeout ] && exit 124
code=$(tr -d '\r' < "$log" | sed -n 's/.*joist: finish code \([0-9]*\).*/\1/p' | tail -1)
[ -n "$code" ] || exit 0
exit $(( (code << 1) | 1 ))
