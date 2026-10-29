# R1: mmbench under KVM (nested on joist-dev2) as E2 (~/vlog4/bench.sh), before
# (~/vlog-rm/base: mmlib.c of 2a5b893) and after (~/joist-rm), boots
# alternating so both see the same load. The default build on 1 and 2 CPUs,
# 3 boots each; then the large-clone build on 1 CPU, 3 boots each.
out=~/vlog-rm/bench; mkdir -p $out
boot() { # ISO LOG MEM SMP
  echo "$(date -u +%T) load $(cut -d" " -f1-3 /proc/loadavg) avail $(awk "/MemAvailable/{print int(\$2/1024)}" /proc/meminfo) MiB: $2"
  timeout 600 qemu-system-x86_64 -M q35 -m $3 -cpu host -enable-kvm -smp $4 -cdrom $1 -display none \
      -no-reboot -serial file:$2 -device isa-debug-exit,iobase=0xf4,iosize=0x04
  echo "boot $2: status $?"
  grep -a "^mmbench\|root task\|^root:" $2
}
for t in base:$HOME/vlog-rm/base after:$HOME/joist-rm; do
  cd ${t#*:}/kernel
  make -s -j2 BUILD=build/bench-mm ROOT_TASK=mmbench MODULES="mm mmclient" USER_BUILD=build/mmbench build/bench-mm/joist.iso || { echo BUILD FAILED; exit 1; }
  make -s -j2 BUILD=build/bench-mm-big ROOT_TASK=mmbench MODULES="mm mmclient" USER_BUILD=build/mmbench-big \
    UEXTRA="-DMMB_POOL=66048u -DMMB_CLONE_PAGES=65536u -DMMB_TABLES=288u" \
    MMFLAGS="-DMM_MAX_TASKS=4u -DMM_TASK_PAGES=66048u -DMM_PAGES=66048u -DMM_TABLE_PAGES=288u" \
    KEXTRA="-DRUN_POOL_FRAMES=2u -DRUN_POOL_PAGES=69632u -DROOT_BUDGET=16384u" \
    KSIZES="-DJOIST_MAX_OBJECTS=8192u -DJOIST_MAX_CSPACES=64u -DJOIST_MAX_THREADS=128u -DJOIST_MAX_LEAVES=256u" \
    build/bench-mm-big/joist.iso || { echo BIG BUILD FAILED; exit 1; }
done
B=$HOME/vlog-rm/base/kernel/build; A=$HOME/joist-rm/kernel/build
for s in 1 2; do for i in 1 2 3; do
  boot $B/bench-mm/joist.iso $out/before-kvm-smp$s-$i.log 256M $s
  boot $A/bench-mm/joist.iso $out/after-kvm-smp$s-$i.log 256M $s
done; done
for i in 1 2 3; do
  boot $B/bench-mm-big/joist.iso $out/before-kvm-smp1-big-$i.log 1024M 1
  boot $A/bench-mm-big/joist.iso $out/after-kvm-smp1-big-$i.log 1024M 1
done
