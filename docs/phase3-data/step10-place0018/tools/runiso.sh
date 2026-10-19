#!/bin/sh
# runiso.sh ISO LOG TIMEOUT [CPUS] : boot.sh.current's remote half, run here
# (this host is the P53; ssh to itself is refused): the same QEMU command
# line, the fixed zeroed RAM file /dev/shm/joist-test-ram, the build VM
# narrowed to 0,1,6,7 for the boot (and back to 0,1,6-11 after, whatever the
# end), QEMU on 0,1,6,7 and vCPU n moved to the n-th host CPU of CPUS
# (default "2 3 4 5", boot.sh's P53_CPUS), under /srv/joist/test/lock. Adds
# only a record of the threads' affinity and last host CPU (read 1 s after
# the move, and again at the end) at the head of LOG.
set -u
iso=$(realpath "$1") log=$(realpath -m "$2") limit=$3
cpus=${4:-2 3 4 5}
name=joist-test-$$.iso
cp "$iso" /srv/joist/test/$name || exit 2
exec 9>/srv/joist/test/lock
flock 9
cd /srv/joist/test || exit 2
rm -f serial.log
cp /usr/share/qemu/edk2-i386-vars.fd vars.fd
[ -f /dev/shm/joist-test-ram ] || : > /dev/shm/joist-test-ram
dd if=/dev/zero of=/dev/shm/joist-test-ram bs=1M count=256 conv=notrunc status=none || exit 2
vm_pin() {
    for vm in $(pgrep -f "^qemu-system-x86_64 -name joist-dev2" || true); do
        for t in /proc/$vm/task/*; do taskset -pc $1 ${t##*/} >/dev/null 2>&1; done
    done
}
vm_pin 0,1,6,7
trap 'vm_pin 0,1,6-11' EXIT
ram="-object memory-backend-file,id=ram0,mem-path=/dev/shm/joist-test-ram,size=256M,share=on,prealloc=on -machine memory-backend=ram0"
timeout $limit taskset -c 0,1,6,7 qemu-system-x86_64 -name joist-test,debug-threads=on -machine q35,accel=kvm \
    -cpu host -smp 4 -m 256M $ram -cdrom $name -display none -vga std -no-reboot \
    -serial file:serial.log -device isa-debug-exit,iobase=0xf4,iosize=0x04 &
qpid=$!
sleep 0.3
n=0
for c in $cpus; do
    for comm in /proc/$(pgrep -P $qpid -x qemu-system-x86 || echo $qpid)/task/*/comm /proc/$qpid/task/*/comm; do
        [ "$(cat $comm 2>/dev/null)" = "CPU $n/KVM" ] && taskset -pc $c $(echo $comm | cut -d/ -f5) >/dev/null 2>&1
    done
    n=$((n + 1))
done
pins() { # $1 label: every QEMU thread's affinity and the host CPU it last ran on
    q=$(pgrep -P $qpid -x qemu-system-x86 || echo $qpid)
    for t in /proc/$q/task/*; do
        echo "pin-$1: $(cat $t/comm 2>/dev/null): $(taskset -pc ${t##*/} 2>/dev/null | sed 's/.*: //') last $(awk '{print $39}' $t/stat 2>/dev/null)"
    done
    echo "pin-$1: build VM: $(for vm in $(pgrep -f '^qemu-system-x86_64 -name joist-dev2'); do taskset -acp $vm | awk '{print $NF}' | sort | uniq -c | tr '\n' ' '; done)"
}
( sleep 1; pins 1s > pins.$$ ) &
pp=$!
wait $qpid
st=$?
wait $pp
{ echo "runiso-status: $st"; echo "runiso-cpus: $cpus"; cat pins.$$; cat serial.log; } > "$log"
rm -f pins.$$ $name
exit $st
