#!/bin/sh
# campaign.sh N [A B] : benchfix run 2's campaign.sh, for two images booted
# alternately (round r: lb-cur then lb-cur-stats, the other way round in even
# rounds) until each has N kept boots. Same idle gate (build VM < 2 % busy
# over 5 s before, and over the whole boot or the boot is repeated), same
# frequency sampler, /proc/interrupts before and after, check.sh before every
# boot (stop if a setting differs). Logs: raw/IMG-N.{log,freq,irq-before,irq-after}.
set -u
h=/srv/joist/handoff/contend
w=/srv/joist/test/contend
runiso=/srv/joist/handoff/lockstudy/tools/runiso.sh
total=$1
A=${2:-lb-cur} B=${3:-lb-cur-stats}
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
kept() { ls $h/raw/ | grep -c -E "^$1-[0-9]+\.log\$"; }
boot() { # $1 = image name; boots it until one is kept
    img=$1
    n=$(( $(kept $img) + 1 ))
    [ $n -gt $total ] && return
    while :; do
        g=$(gov)
        case "$g" in "performance performance performance performance ") ;;
            *) echo "$(date +%T) governor not performance: $g; stopping" >> $h/raw/campaign.log; exit 3;; esac
        if ! $h/tools/check.sh > $w/check.out 2>&1; then
            { echo "$(date +%T) setting differs, stopping:"; cat $w/check.out; } >> $h/raw/campaign.log; exit 4; fi
        wait_idle
        echo "measuring: $img run $n of $total" > $h/status
        a=$(vmstat_)
        cat /proc/interrupts > $w/cur.irq-before
        $runiso $h/iso/$img.iso $w/cur.log 400 &
        p=$!
        sampler $p > $w/cur.freq
        wait $p; st=$?
        cat /proc/interrupts > $w/cur.irq-after
        b=$(vmstat_)
        x=$(busy "$a" "$b")
        g2=$(gov)
        echo "$(date +%T) $img-$n status $st vm-busy $x % governor-after: $g2" >> $h/raw/campaign.log
        if [ "$(echo "$x < 2" | bc)" = 1 ]; then
            { echo "campaign: $(date -u +%FT%TZ) build VM busy during run: $x %; governor before: $g after: $g2"; cat $w/cur.log; } > $h/raw/$img-$n.log
            mv $w/cur.freq $h/raw/$img-$n.freq
            mv $w/cur.irq-before $h/raw/$img-$n.irq-before; mv $w/cur.irq-after $h/raw/$img-$n.irq-after
            return
        fi
        mv $w/cur.log $w/discarded-$(date +%s).log
        mv $w/cur.freq $w/discarded-$(date +%s).freq
    done
}
$h/tools/check.sh > $h/raw/check-before-$B.txt 2>&1 || { echo "$(date +%T) settings differ at start" >> $h/raw/campaign.log; exit 4; }
r=0
while [ $(kept $A) -lt $total ] || [ $(kept $B) -lt $total ]; do
    r=$((r + 1))
    if [ $((r % 2)) = 1 ]; then boot $A; boot $B; else boot $B; boot $A; fi
done
$h/tools/check.sh > $h/raw/check-after-$B.txt 2>&1; echo "check-after exit $?" >> $h/raw/campaign.log
echo "$(date +%T) campaign finished" >> $h/raw/campaign.log
