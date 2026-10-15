#!/bin/sh
# campaign.sh ROUNDS VARIANT... : boot each variant's ISO once per round
# (variants interleaved), each boot only while the build VM is idle, and
# kept only if the VM stayed idle through it (its /proc/stat busy share,
# all 8 vCPUs, under 2 %); otherwise booted again. Logs to
# /srv/joist/handoff/timerout/raw/VARIANT-N.log.
# With QMP=1 also KVM statistics (raw/VARIANT-N.kvmstats).
set -u
h=/srv/joist/handoff/timerout
w=/srv/joist/test/timerout
rounds=$1; shift
vmstat_() { ssh -p 2222 -o BatchMode=yes aditya@127.0.0.1 'head -1 /proc/stat'; }
busy() { # two "cpu ..." lines: busy share in per cent
    echo "$1
$2" | awk '{t=0; for(i=2;i<=NF;i++) t+=$i; idle=$5+$6; if (NR==1){t0=t;i0=idle} else {printf "%.1f", 100*((t-t0)-(idle-i0))/(t-t0)}}'
}
wait_idle() {
    while :; do
        a=$(vmstat_); sleep 5; b=$(vmstat_)
        x=$(busy "$a" "$b")
        [ "$(echo "$x < 2" | bc)" = 1 ] && return
        echo "$(date +%T) build VM busy ($x %), waiting" >> $w/campaign.log
        echo "measuring: waiting for the build VM to be idle ($x % busy)" > $h/status
        sleep 50
    done
}
for r in $(seq 1 $rounds); do
    for v in "$@"; do
        n=$(ls $h/raw/ | grep -c -E "^$v-[0-9]+\.log$")
        n=$((n + 1))
        while :; do
            wait_idle
            echo "measuring: $v run $n (round $r of $rounds)" > $h/status
            a=$(vmstat_)
            q=""
            [ "${QMP:-0}" = 1 ] && q=$h/raw/$v-$n.kvmstats
            $w/runiso.sh $w/iso/$v.iso $w/cur.log ${LIMIT:-400} $q
            st=$?
            b=$(vmstat_)
            x=$(busy "$a" "$b")
            echo "$(date +%T) $v-$n status $st vm-busy $x %" >> $w/campaign.log
            if [ "$(echo "$x < 2" | bc)" = 1 ]; then
                { echo "campaign: $(date -u +%FT%TZ) build VM busy during run: $x %"; cat $w/cur.log; } > $h/raw/$v-$n.log
                break
            fi
            mv $w/cur.log $w/discarded-$v-$(date +%s).log
            [ -n "$q" ] && rm -f "$q"
        done
    done
done
