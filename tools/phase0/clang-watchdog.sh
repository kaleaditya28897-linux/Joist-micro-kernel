#!/bin/bash
# Stop any single clang compile that exceeds a CPU-time cap, and log it, so a
# pathological compile cannot stall the glibc spike. Used for decision 0002 on
# VM 111 (2026-09-30), where `wcstold_l.c` compiled with -fPIC ran 30 CPU-min.
# usage: clang-watchdog.sh CAP_MINUTES LOGFILE     (exits when no make is left)
cap=${1:-30}; log=${2:-$HOME/spike/clang-watchdog.log}
cpu_min() {  # ps TIME is [[H:]M]M:SS.ss on the Hurd
  awk -F: '{ if (NF == 3) print $1 * 60 + $2; else print $1 + 0 }' <<< "$1"; }
while :; do
  if ! timeout 30 ps -e -o pid,time,args > /tmp/wd.ps 2>/dev/null; then sleep 20; continue; fi
  grep -qE ' make ' /tmp/wd.ps || { echo "$(date +%F\ %T) no make left, exiting" >> "$log"; exit 0; }
  grep -E '^ *[0-9]+ +[0-9:.]+ +clang ' /tmp/wd.ps | while read -r pid t args; do
    if [ "$(cpu_min "$t")" -ge "$cap" ]; then
      kill -TERM "$pid" && echo "$(date +%F\ %T) killed pid $pid after $t CPU: ${args:0:300}" >> "$log"
    fi
  done
  sleep 30
done
