#!/bin/sh
# runiso-mem.sh ISO LOG TIMEOUT : lockstudy runiso.sh (same QEMU command line), but with
# MEMFILE set, guest RAM is a file there (memory-backend-file, share=on), kept between boots so
# the host pages behind the guest stay the same. Also records the QEMU process's smaps_rollup.
# (original header:) runiso.sh ISO LOG TIMEOUT [QMPSTATS]
# Boot a Joist ISO on the P53 through KVM, as tools/p53/boot.sh does (SeaBIOS,
# -smp 4, vCPU n pinned to host CPU 2+n, under /srv/joist/test/lock).
# With QMPSTATS set, KVM's per-vCPU statistics are read through QMP
# (query-stats) once a second into QMPSTATS (the last reads before exit).
set -u
iso=$(realpath "$1") log=$(realpath -m "$2") limit=$3 qstats=${4:+$(realpath -m "$4")}
smp=${LS_SMP:-4}
cpus=${LS_CPUS:-2 3 4 5}
d=/srv/joist/test/lockstudy/run.$$
mkdir -p $d
cp "$iso" $d/x.iso
exec 9>/srv/joist/test/lock
flock 9
cd $d || exit 2
qmp=""
memopt=""
[ -n "${MEMFILE:-}" ] && memopt="-object memory-backend-file,id=ram0,size=256M,mem-path=$MEMFILE,share=on -machine memory-backend=ram0"
[ -n "$qstats" ] && qmp="-qmp unix:$d/qmp.sock,server=on,wait=off"
timeout $limit qemu-system-x86_64 -name joist-test,debug-threads=on -machine q35,accel=kvm \
    -cpu host -smp $smp -m 256M $memopt -cdrom x.iso -display none -vga std -no-reboot \
    -serial file:serial.log -device isa-debug-exit,iobase=0xf4,iosize=0x04 $qmp &
tpid=$!
sleep 0.3
qpid=$(pgrep -P $tpid -x qemu-system-x86 || echo $tpid)
n=0
for c in $cpus; do
    for comm in /proc/$qpid/task/*/comm; do
        [ "$(cat $comm 2>/dev/null)" = "CPU $n/KVM" ] && taskset -pc $c $(echo $comm | cut -d/ -f5) >/dev/null 2>&1
    done
    n=$((n + 1))
done
# record the pinning
for comm in /proc/$qpid/task/*/comm; do
    tid=$(echo $comm | cut -d/ -f5)
    echo "pin: $(cat $comm 2>/dev/null): $(taskset -pc $tid 2>/dev/null | sed 's/.*: //')"
done > pins.txt
( sleep 1.5; echo "memfile: ${MEMFILE:-none}"; grep -E "^(Rss|AnonHugePages|Shmem|ShmemPmdMapped|FilePmdMapped):" /proc/$qpid/smaps_rollup | sed "s/^/smaps: /" ) > smaps.txt &
if [ -n "$qstats" ]; then
    python3 -I /srv/joist/test/lockstudy/qmpstats.py $d/qmp.sock "$qstats" &
fi
wait $tpid
st=$?
echo "runiso-status: $st" > "$log"
cat pins.txt >> "$log"; cat smaps.txt >> "$log" 2>/dev/null
cat serial.log >> "$log"
cd / && rm -rf $d
exit $st
