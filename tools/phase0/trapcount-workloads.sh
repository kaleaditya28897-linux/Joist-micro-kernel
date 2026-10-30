#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Decision 0014: count the Mach yield traps (swtch, swtch_pri, thread_switch)
# on the same three workloads as workloads.sh, untraced. Requires the libc0.3
# built with tools/phase0/glibc-trapcount.patch to be installed. Each process
# that ends through exit() appends "pid program swtch swtch_pri thread_switch"
# to $JOIST_TRAPCOUNT. Processes started before the install (running daemons)
# keep the old libc and are not counted.
# usage: trapcount-workloads.sh OUTDIR   (OUTDIR gets boot.log shell.log build.log)
out=$(realpath -m "${1:-$HOME/trapcount}"); mkdir -p "$out"; cd "$out" || exit 1
rm -f boot.log shell.log build.log

# 1. boot proxy: start an ext2fs translator on an image, use it, tear it down.
rm -f fs.img; dd if=/dev/zero of=fs.img bs=1M count=64 status=none
/sbin/mke2fs -q -o hurd fs.img 2>/dev/null; mkdir -p mnt
JOIST_TRAPCOUNT=$out/boot.log sh -c '
  settrans -ca mnt /hurd/ext2fs "$PWD/fs.img"
  mkdir mnt/d; echo hi > mnt/d/f; cat mnt/d/f; ls -l mnt
  settrans -g mnt' > boot.out 2>&1
echo "boot rc=$?"

# 2. shell: the same ten commands as workloads.sh.
while read -r c; do
  JOIST_TRAPCOUNT=$out/shell.log sh -c "$c" > /dev/null 2>&1
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
echo "shell done"

# 3. package build: hello, from a fresh source tree.
rm -rf hello-2.12.3 hello_*; apt-get source -q hello > /dev/null 2>&1
( cd hello-2.12.3 && JOIST_TRAPCOUNT=$out/build.log dpkg-buildpackage -us -uc -b > ../build.out 2>&1 )
echo "build rc=$?"

for w in boot shell build; do
  [ -f "$w.log" ] || { echo "$w: no log"; continue; }
  awk -v w="$w" '{p++; s+=$3; sp+=$4; ts+=$5; if ($3+$4+$5) n++}
       END {printf "%-6s processes=%-6d with-yield=%-5d swtch=%-8d swtch_pri=%-8d thread_switch=%d\n", w, p, n, s, sp, ts}' "$w.log"
done
