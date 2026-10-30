#!/usr/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# guest-init.sh - init for a job in live-bootstrap's final system
# (tools/ddc/guest.sh copies it to /in/init.sh). Mounts what a build
# needs, runs /in/job.sh with the work disk at /work, records its output
# and status there, and powers the guest off.
export PATH=/usr/bin:/usr/sbin LC_ALL=C TZ=UTC HOME=/root
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount | grep -q ' /dev ' || mount -t devtmpfs none /dev
mkdir -p /work /tmp
mount -t ext3 /dev/sdc /work
mount -t tmpfs tmpfs /tmp
mkswap /dev/sdd >/dev/null && swapon /dev/sdd
export TMPDIR=/work/tmp
rm -rf "$TMPDIR"
mkdir -p "$TMPDIR"
rm -f /work/job.status

echo "guest: job starting, $(nproc) CPUs, $(date -u)"
cd /work
/usr/bin/bash /in/job.sh 2>&1 | tee /work/job.log
st=${PIPESTATUS[0]}
echo "guest: job finished, status $st, $(date -u)"
echo "$st" > /work/job.status

cd /
swapoff /dev/sdd
sync
umount /work
echo o > /proc/sysrq-trigger
while true; do sleep 1; done
