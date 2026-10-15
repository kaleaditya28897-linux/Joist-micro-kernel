#!/bin/sh
# stress.sh: every stress boot once (fi-N, fichk-N) / 5 times (smp, sched),
# 4 vCPUs pinned 2-5 (runiso.sh). Every boot kept; verdicts to stress.txt.
set -u
h=/srv/joist/handoff/timerout
w=/srv/joist/test/timerout
fi_pats='faultinj: call ok|faultinj: server ok|faultinj: saved ok|faultinj: transfer ok|faultinj: signal ok|faultinj: 13 of 13 kill points covered|faultinj: all ok|root task exited with code 0'
boot() { # NAME RUN PATTERNS
    v=$1; n=$2; pats=$3
    echo "stress: $v run $n" > $h/status
    log=$h/raw/stress-$v-$n.log
    t0=$(date +%s)
    $w/runiso.sh $w/iso/$v.iso $log 300
    st=$?
    t=$(( $(date +%s) - t0 ))
    ok=ok; why=""
    [ $st -eq 1 ] || { ok=FAILED; why="status $st"; }
    IFS='|'; for p in $pats; do grep -a -q -- "$p" $log || { ok=FAILED; why="$why; missing '$p'"; }; done; unset IFS
    echo "$v run $n: $ok (QEMU status $st, ${t}s)$why" | tee -a $w/stress.txt
}
for s in $(seq 21 30); do
    boot fi-$s 1 "$fi_pats"
    boot fichk-$s 1 "$fi_pats"
done
for r in 1 2 3 4 5; do
    boot to-smp $r "cpus in use: 4|smp: all ok|root task exited with code 0"
    boot to-sched $r "cpus in use: 4|sched: all ok|root task exited with code 0"
done
echo "stress finished $(date +%T)" >> $w/stress.txt
