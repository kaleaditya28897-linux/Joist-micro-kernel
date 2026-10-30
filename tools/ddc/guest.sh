#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# guest.sh - run a job in live-bootstrap's final system (decision 0026,
# layer 3), under QEMU with KVM, and give its exit status.
#
# The bootstrap's disk is never written: the guest boots a throwaway
# copy-on-write layer over it, with the kernel the bootstrap built. Three
# more disks: INPUT, a new ext3 disk with the files of INDIR (job.sh and
# what it needs; mounted read-only at /in), WORK, an ext3 disk that is
# kept from job to job (mounted at /work; made if missing), and a swap
# disk. The guest has no network. guest-init.sh runs /in/job.sh, writes its
# output to the console (LOG) and /work/job.log and its status to
# /work/job.status, and powers off.
#
# usage: tools/ddc/guest.sh IMAGE WORK INDIR LOG
#   IMAGE  the bootstrap's final disk (target/external.img), read-only
# Environment: CORES (default 6), RAM in MiB (default 6144), WORK_SIZE
# (default 80G), SWAP_SIZE (default 8G), LIMIT in seconds (default 36000).
set -eu
image=$1 work=$2 indir=$3 log=$4
here=$(cd "$(dirname "$0")" && pwd)
cores=${CORES:-6}
ram=${RAM:-6144}
PATH=$PATH:/usr/sbin:/sbin # debugfs, mkfs.ext3
tmp=$(mktemp -d "${TMPDIR:-/tmp}/ddc-guest.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
# The bootstrap's root file system starts 1 MiB into its disk
# (live-bootstrap's lib/utils.py, create_disk).
root="$image?offset=1048576"

debugfs -R "dump /boot/vmlinuz $tmp/vmlinuz" "$root" 2>/dev/null
[ -s "$tmp/vmlinuz" ] || { echo "guest: no /boot/vmlinuz in $image"; exit 1; }
qemu-img create -q -f qcow2 -F raw -b "$(cd "$(dirname "$image")" && pwd)/$(basename "$image")" \
    "$tmp/root.qcow2"

mkdir "$tmp/in"
cp -R "$indir"/. "$tmp/in"/
cp "$here/guest-init.sh" "$tmp/in/init.sh"
size=$(( $(du -sm "$tmp/in" | cut -f1) * 11 / 10 + 64 ))
truncate -s "${size}M" "$tmp/in.img"
mkfs.ext3 -q -F -d "$tmp/in" "$tmp/in.img"

if [ ! -e "$work" ]; then
    truncate -s "${WORK_SIZE:-80G}" "$work"
    mkfs.ext3 -q -F "$work"
fi
truncate -s "${SWAP_SIZE:-8G}" "$tmp/swap.img"

# Kernel arguments after "--" go to init: bash runs the job's init.
timeout "${LIMIT:-36000}" qemu-system-x86_64 -enable-kvm -machine pc -nodefaults \
    -m "$ram" -smp "$cores" -display none -no-reboot -nic none \
    -serial file:"$log" \
    -drive file="$tmp/root.qcow2",format=qcow2,if=ide,index=0 \
    -drive file="$tmp/in.img",format=raw,if=ide,index=1 \
    -drive file="$work",format=raw,if=ide,index=2 \
    -drive file="$tmp/swap.img",format=raw,if=ide,index=3 \
    -kernel "$tmp/vmlinuz" \
    -append 'root=/dev/sda1 rw console=ttyS0 init=/usr/bin/bash -- -c "mkdir -p /in && mount -t ext3 -o ro /dev/sdb /in && exec /usr/bin/bash /in/init.sh"' ||
    { echo "guest: QEMU failed or timed out ($?)"; exit 1; }

st=$(debugfs -R "cat /job.status" "$work" 2>/dev/null)
echo "guest: job.sh exit status: ${st:-none}"
[ "$st" = 0 ]
