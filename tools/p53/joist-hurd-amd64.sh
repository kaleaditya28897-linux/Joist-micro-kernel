#!/bin/sh
# joist-hurd-amd64.sh - the GNU/Hurd amd64 testbed on the P53 (docs/testbed.md,
# VM 111 until 2026-10-14). Run by hand when it is needed:
#   /srv/joist/vms/joist-hurd-amd64/run.sh &      (as aditya on the P53)
# Hurd's disk driver wants AHCI, its network an e1000: as on Proxmox, with
# the same MAC. SSH on the P53's loopback port 2223 (`ssh joist-amd64`).
#
# smm=off: on the P53, under KVM, SeaBIOS with SMM resets the guest as GRUB
# hands over to the kernel (found 2026-10-14, with Linux and Hurd alike);
# without SMM it boots. Hurd needs no SMM.
dir=/srv/joist/vms/joist-hurd-amd64
exec qemu-system-x86_64 -name joist-hurd-amd64,debug-threads=on \
    -machine pc,accel=kvm,smm=off -cpu host -smp 2 -m 4096 \
    -device ich9-ahci,id=ahci \
    -drive file=$dir/disk.qcow2,if=none,id=d0,cache=writeback \
    -device ide-hd,drive=d0,bus=ahci.0 \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2223-:22 \
    -device e1000,netdev=n0,mac=02:00:00:00:00:02 \
    -vga std -display none \
    -serial unix:$dir/console.sock,server=on,wait=off \
    -monitor unix:$dir/monitor.sock,server=on,wait=off
