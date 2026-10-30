#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
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
# Pinned to host CPUs 0, 1 and 6-11 (taskset, every thread): never 2-5,
# where tools/p53/boot.sh pins test boots. 8-11 are those cores' second
# threads, so for each test boot boot.sh narrows this VM to 0, 1, 6 and 7
# (two cores) and widens it again after: the build VM never runs beside a
# benchmark, and has six cores' worth of threads the rest of the time (an
# emulated 4-CPU self-test needs more than two cores; 2026-10-15 and -17;
# see tools/p53/rc.local-bench and docs/testbed.md).
#
# 12 GB of memory (8 until 2026-10-29: CBMC's proofs of the scheduling calls
# with two threads ran out) and a 100 GB disk (40 until the same day, grown
# online with QEMU's block_resize, then growpart and resize2fs in the guest:
# room for decision 0026's bootstrap and LLVM builds).
#
# UEFI (OVMF, its variables in efivars.fd), not SeaBIOS as on Proxmox: on
# the P53, under KVM, a SeaBIOS boot of Linux resets the guest at the
# kernel's first instruction (found 2026-10-14; the same disk boots by
# UEFI). The Debian cloud image has both boot paths.
dir=/srv/joist/vms/joist-dev2
exec taskset -c 0,1,6-11 qemu-system-x86_64 -name joist-dev2,debug-threads=on \
    -machine q35,accel=kvm -cpu host -smp 8 -m 12288 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/qemu/edk2-x86_64-code.fd \
    -drive if=pflash,format=raw,file=$dir/efivars.fd \
    -drive file=$dir/disk.qcow2,if=virtio,discard=unmap,cache=writeback \
    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22 \
    -device virtio-net-pci,netdev=n0,mac=02:00:00:00:00:03 \
    -display none -vga none \
    -serial unix:$dir/console.sock,server=on,wait=off \
    -monitor unix:$dir/monitor.sock,server=on,wait=off
