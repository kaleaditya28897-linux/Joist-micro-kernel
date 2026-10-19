#!/bin/sh
# runiso-cur.sh ISO LOG TIMEOUT : boot.sh.current's remote half, run here (this
# host is the P53; ssh to itself is refused): the same QEMU command line, the
# fixed zeroed RAM file /dev/shm/joist-test-ram, QEMU on 0,1,6,7 and vCPU n
# moved to host CPU 2+n, under /srv/joist/test/lock. Adds only a record of the
# threads' affinity (read 1 s after the move) at the head of LOG.
set -u
iso=$(realpath "$1") log=$(realpath -m "$2") limit=$3
cpus="2 3 4 5"
name=joist-test-$$.iso
cp "$iso" /srv/joist/test/$name || exit 2
exec 9>/srv/joist/test/lock
flock 9
cd /srv/joist/test || exit 2
rm -f serial.log
cp /usr/share/qemu/edk2-i386-vars.fd vars.fd
[ -f /dev/shm/joist-test-ram ] || : > /dev/shm/joist-test-ram
dd if=/dev/zero of=/dev/shm/joist-test-ram bs=1M count=256 conv=notrunc status=none || exit 2
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
( sleep 1; q=$(pgrep -P $qpid -x qemu-system-x86 || echo $qpid)
  for comm in /proc/$q/task/*/comm; do
      echo "pin: $(cat $comm 2>/dev/null): $(taskset -pc $(echo $comm | cut -d/ -f5) 2>/dev/null | sed 's/.*: //')"
  done > pins.$$ ) &
wait $qpid
st=$?
wait
{ echo "runiso-status: $st"; cat pins.$$; cat serial.log; } > "$log"
rm -f pins.$$ $name
exit $st
