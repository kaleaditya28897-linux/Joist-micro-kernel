#!/bin/sh
# joist-dev2.sh - the build and test VM on the P53 (docs/testbed.md), run by
# runit as the user aditya (tools/p53/sv/joist-dev2). Installed as
# /srv/joist/vms/joist-dev2/run.sh.
#
# Debian 13, its disk moved from Proxmox (pvehome VM 114) on 2026-10-14.
# Same MAC as there, so the guest's network set-up still matches; QEMU's
# user-mode network, its SSH port on the P53's loopback only (2222): reached
# as `ssh joist-dev2` through the P53. Nested KVM works inside (-cpu host),
# for the self-tests that need a real CPU.
#
# UEFI (OVMF, its variables in efivars.fd), not SeaBIOS as on Proxmox: on
# the P53, under KVM, a SeaBIOS boot of Linux resets the guest at the
# kernel's first instruction (found 2026-10-14; the same disk boots by
# UEFI). The Debian cloud image has both boot paths.
dir=/srv/joist/vms/joist-dev2
exec qemu-system-x86_64 -name joist-dev2,debug-threads=on \
    -machine q35,accel=kvm -cpu host -smp 8 -m 8192 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/qemu/edk2-x86_64-code.fd \
    -drive if=pflash,format=raw,file=$dir/efivars.fd \
    -drive file=$dir/disk.qcow2,if=virtio,discard=unmap,cache=writeback \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=n0,mac=02:00:00:00:00:03 \
    -display none -vga none \
    -serial unix:$dir/console.sock,server=on,wait=off \
    -monitor unix:$dir/monitor.sock,server=on,wait=off
