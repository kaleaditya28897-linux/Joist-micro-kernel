#!/bin/sh
# campaign.sh N : boot iso/fast.iso N times through lockstudy's runiso.sh,
# each boot only while the build VM is < 2 % busy, kept only if it stayed so.
# While each boot runs, samples scaling_cur_freq of host CPUs 2-5 every 0.2 s
# (column 2: 1 if this boot's QEMU process exists, else 0 = lock wait/teardown).
# Logs: raw/fast-N.log, raw/fast-N.freq, raw/campaign.log.
# run2 additions: /proc/interrupts snapshot just before and after each boot
# (raw/fast-N.irq-before/-after), and tools/check.sh before every boot (stop if a
# setting differs; output in raw/campaign.log).
set -u
h=/srv/joist/handoff/benchfix
w=/srv/joist/test/benchfix
runiso=/srv/joist/handoff/lockstudy/tools/runiso.sh
total=$1
vmstat_() { ssh -p 2222 -o BatchMode=yes aditya@127.0.0.1 'head -1 /proc/stat'; }
busy() {
    echo "$1
$2" | awk '{t=0; for(i=2;i<=NF;i++) t+=$i; idle=$5+$6; if (NR==1){t0=t;i0=idle} else {printf "%.1f", 100*((t-t0)-(idle-i0))/(t-t0)}}'
}
gov() { cat /sys/devices/system/cpu/cpu[2-5]/cpufreq/scaling_governor | tr '\n' ' '; }
wait_idle() {
    while :; do
        a=$(vmstat_); sleep 5; b=$(vmstat_)
        x=$(busy "$a" "$b")
        [ "$(echo "$x < 2" | bc)" = 1 ] && return
        echo "$(date +%T) build VM busy ($x %), waiting" >> $h/raw/campaign.log
        echo "measuring: waiting for the build VM to be idle ($x % busy)" > $h/status
        sleep 50
    done
}
sampler() { # $1 = runiso pid
    while kill -0 $1 2>/dev/null; do
        t=$(pgrep -P $1 -x timeout | head -1)
        q=0; [ -n "$t" ] && pgrep -P $t >/dev/null && q=1
        echo "$(date +%s.%N | cut -c1-14) $q $(cat /sys/devices/system/cpu/cpu[2-5]/cpufreq/scaling_cur_freq | tr '\n' ' ')"
        sleep 0.2
    done
}
n=$(ls $h/raw/ | grep -c -E '^fast-[0-9]+\.log$')
while [ $n -lt $total ]; do
    n=$((n + 1))
    while :; do
        g=$(gov)
        case "$g" in "performance performance performance performance ") ;;
            *) echo "$(date +%T) governor not performance: $g; stopping" >> $h/raw/campaign.log; exit 3;; esac
        if ! $h/tools/check.sh > $w/check.out 2>&1; then
            { echo "$(date +%T) setting differs, stopping:"; cat $w/check.out; } >> $h/raw/campaign.log; exit 4; fi
        wait_idle
        echo "measuring: fast run $n of $total" > $h/status
        a=$(vmstat_)
        cat /proc/interrupts > $w/cur.irq-before
        $runiso $h/iso/fast.iso $w/cur.log 400 &
        p=$!
        sampler $p > $w/cur.freq
        wait $p; st=$?
        cat /proc/interrupts > $w/cur.irq-after
        b=$(vmstat_)
        x=$(busy "$a" "$b")
        g2=$(gov)
        echo "$(date +%T) fast-$n status $st vm-busy $x % governor-after: $g2" >> $h/raw/campaign.log
        if [ "$(echo "$x < 2" | bc)" = 1 ]; then
            { echo "campaign: $(date -u +%FT%TZ) build VM busy during run: $x %; governor before: $g after: $g2"; cat $w/cur.log; } > $h/raw/fast-$n.log
            mv $w/cur.freq $h/raw/fast-$n.freq
            mv $w/cur.irq-before $h/raw/fast-$n.irq-before; mv $w/cur.irq-after $h/raw/fast-$n.irq-after
            break
        fi
        mv $w/cur.log $w/discarded-$(date +%s).log
        mv $w/cur.freq $w/discarded-$(date +%s).freq
    done
done
echo "$(date +%T) campaign finished" >> $h/raw/campaign.log
