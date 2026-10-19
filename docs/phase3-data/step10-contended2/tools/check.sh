#!/bin/sh
# check.sh : print the host settings the other session made and exit 1 if any differs:
# governor of CPUs 2-5 = performance; default_smp_affinity = c3; no IRQ outside the
# allowed list (IRQs that could not be steered at the start) includes CPUs 2-5 or 8-11;
# build VM QEMU pid 9937 and every thread on 0,1,6,7.
allowed=" 2 134 135 136 137 140 141 142 143 179 196 "
bad=0
g=$(cat /sys/devices/system/cpu/cpu[2-5]/cpufreq/scaling_governor | tr '\n' ' ')
echo "governor cpu2-5: $g"
[ "$g" = "performance performance performance performance " ] || { echo "DIFFERS: governor"; bad=1; }
d=$(cat /proc/irq/default_smp_affinity)
echo "default_smp_affinity: $d"
[ "$(printf %x 0x$d)" = c3 ] || { echo "DIFFERS: default_smp_affinity"; bad=1; }
for f in /proc/irq/*/smp_affinity_list; do
    i=$(basename $(dirname $f)); l=$(cat $f)
    # expand list to CPUs
    cpus=$(echo $l | tr ',' '\n' | awk -F- '{b=$1; e=(NF>1?$2:$1); for(c=b;c<=e;c++) print c}')
    if echo "$cpus" | grep -qxE '[2-5]|8|9|10|11'; then
        name=$(grep -E "^ *$i:" /proc/interrupts | awk '{print $NF}')
        echo "irq $i affinity $l effective $(cat /proc/irq/$i/effective_affinity_list 2>/dev/null) ${name:-(not in /proc/interrupts)}"
        case "$allowed" in *" $i "*) ;; *) echo "DIFFERS: irq $i"; bad=1;; esac
    fi
done
if [ -d /proc/9937 ] && grep -q joist-dev2 /proc/9937/cmdline; then
    t=$(taskset -acp 9937 | awk '{print $NF}' | sort -u | tr '\n' ' ')
    echo "build VM qemu 9937: $(ls /proc/9937/task | wc -l) threads, affinity lists: $t"
    [ "$t" = "0,1,6,7 " ] || { echo "DIFFERS: build VM pinning"; bad=1; }
else echo "DIFFERS: pid 9937 is not joist-dev2's QEMU"; bad=1; fi
exit $bad
