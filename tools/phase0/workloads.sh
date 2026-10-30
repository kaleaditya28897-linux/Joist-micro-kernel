#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Phase 0 rpctrace workloads. Run from ~/trace on the testbed
# (joist-hurd as demo on i386, 2026-09-23; joist-amd64 as aditya on x86_64, 2026-09-30).
# RPCTRACE: the rpctrace command (default: system rpctrace). TRACEDIR: where
# traces go (default ~/trace). The 2026-10-01 re-run used the gsync-patched
# rpctrace (tools/phase0/rpctrace-gsync.patch) with -I /usr/share/msgids.
R=${RPCTRACE:-rpctrace}; T=${TRACEDIR:-$HOME/trace}
cd "$T" || exit 1
S=16   # truncate string args; we only need call names

# 1. "boot" proxy: bring up a fresh ext2fs translator on an image, use it, tear it down.
#    rpctrace cannot follow the real boot (it wraps a task it starts); translator
#    startup is the bulk of what boot does after the kernel hands off.
rm -f fs.img; dd if=/dev/zero of=fs.img bs=1M count=64 status=none; /sbin/mke2fs -q -o hurd fs.img
mkdir -p mnt
timeout 300 $R -s $S -o boot.txt sh -c '
  settrans -ca mnt /hurd/ext2fs "$PWD/fs.img"
  mkdir mnt/d; echo hi > mnt/d/f; cat mnt/d/f; ls -l mnt
  settrans -g mnt'
echo "boot rc=$?"

# 2. Shell session. Tracing one long-lived bash deadlocks rpctrace in gsync_wait
#    (see docs/mach-inventory.md), so each command of the session is traced
#    as its own process and the logs are concatenated.
rm -f shell.txt
while read -r c; do
  rm -f one.txt   # else a trace that fails to start re-appends the previous one
  timeout 60 $R -s $S -o one.txt sh -c "$c" >/dev/null 2>&1 || echo "timeout: $c"
  cat one.txt >> shell.txt
done <<'EOF2'
cd /usr/share/doc && ls | head -5
grep -r -l GPL /usr/share/doc/bash
for i in 1 2 3; do echo $i > /tmp/x$i; done; cat /tmp/x* | sort -r
ps aux
echo $(date) | sed s/2026/YEAR/
cp /etc/passwd /tmp/p && rm /tmp/p
sleep 1
ls -l /dev | wc -l
id; whoami; uname -a
bash -c "echo hi"
EOF2

# 3. Package build (needs deb-src for the main debian archive; debian-ports has no sources).
#    apt-get build-dep hello && apt-get source hello, first.
cd hello-2.12.3 && git clean -fdxq 2>/dev/null
timeout 3000 $R -s $S -o ../build.txt dpkg-buildpackage -us -uc -b > ../build.out 2>&1
echo "build rc=$?"

cd "$T"
python3 ~/trace/tally.py /usr/share/msgids/gnumach.msgids boot.txt shell.txt build.txt > mach-call-counts.md
python3 ~/trace/hurdrpc.py boot.txt shell.txt build.txt
