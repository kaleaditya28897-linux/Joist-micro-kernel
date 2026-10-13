# Testbeds

Three live VMs. On **pvehome**: VM 111, the Debian GNU/Hurd testbed for
measurements, and VM 112, which boots the Joist kernel itself (and runs the
benchmarks). On **pve1**: VM 113, `joist-dev`, where everything is built and
tested since 2026-10-09. A fourth, VM 110, is retired.

## VM 113: `joist-dev`, the build and test VM (pve1)

Created 2026-10-09, when the workstation moved from Void (glibc) to Alpine
(musl): Alpine has clang 21 but its sanitizer runtimes and `scan-build` only
for clang 22, and no CBMC, while decision 0002 pins clang 21. Rather than
change the pin or the workstation, the toolchain lives in a Debian VM; the
workstation only edits, commits and pushes.

| | |
|---|---|
| Host | pve1 (cluster `kale-cluster` with pve2): Xeon E5-2670 (Sandy Bridge), 16 threads, 31 GB, ZFS |
| VM | VMID **113**, `joist-dev`; q35, `cpu host` (nested KVM: `/dev/kvm` works inside), 12 vCPUs, 16 GB, 60 GB on `local-zfs`, `onboot 1` |
| OS | Debian 13.7 from the official `debian-13-genericcloud-amd64.qcow2` (SHA-512 checked against Debian's `SHA512SUMS`), cloud-init user `aditya` with the workstation's key |
| Address | <lan-address> on pve1's LAN (DHCP lease); reached as `ssh joist-dev`, jumping through `root@pve1` (<pve1-address>) |

Toolchain, each pinned to what the project used before:

| Tool | Version | Source, verification |
|---|---|---|
| clang, LLD, clang-tidy, clang-format, `scan-build`, LLVM tools, sanitizer and libFuzzer runtimes | **21.1.8** | apt.llvm.org `llvm-toolchain-trixie-21`, key fingerprint `6084 F3CF 814B 57C1 CF12 EFD5 15CF 4D18 AF4F 7421`; `clang`, `ld.lld` etc. linked in `/usr/local/bin` |
| Limine | **12.9.2** | GitHub release `limine-binary.tar.xz`, SHA-256 `07d01fd0139f76afd510d9a8d7805af730ef7f6a55eb5cf23500f132283829de`, signature good from Mintsuki, key `05D2 9860 D0A0 668A AEFB 9D69 1F3C 021B ECA2 3821`; files in `/usr/local/share/limine` (linked as `/usr/share/limine`), the tool built with clang 21 |
| CBMC | **6.11.0** | `ubuntu-24.04-cbmc-6.11.0-Linux.deb`, SHA-256 as in `docs/phase1-plan.md` |
| TLA+ tools | **v1.7.4** (TLC 2.19) | `tla2tools.jar`, SHA-256 as in `docs/phase1-plan.md`; `/usr/local/bin/tlc` wraps it |
| QEMU | 10.0.13 | Debian `1:10.0.13+ds-0+deb13u1` (the workstation had 11.1.1; the kernel's tests don't depend on the difference) |
| OVMF, xorriso, gdb, OpenJDK | 2025.02, 1.5.6, 16.3, 21.0.12 | Debian `2025.02-8+deb13u1`, `1.5.6-1.2+b1`, `16.3-1`, `21.0.12.1+1-1~deb13u1` |

**How it's used:** `tools/dev/remote.sh 'command'` copies the working tree as
it is (uncommitted edits included; `.git` and build directories excluded,
so builds there stay incremental) to `~/joist` on the VM and runs the
command there, e.g. `tools/dev/remote.sh 'make -C model check'`. The first
run there of `make check` in `model/` and in `kernel/` passed in full
(2026-10-09, 21 minutes for both). `make mutate` runs its mutations in
parallel there (`MUTATE_JOBS`, default all CPUs): all 80 in 13 minutes,
against about an hour one at a time on the workstation.


## VM 114: `joist-dev2`, the build and test VM on pvehome

Created 2026-10-13, when pve1 went out of service (and VM 113 with it). The
same recipe as VM 113, on the host that also runs VM 112; `tools/dev/remote.sh`
uses it by default from that date (`JOIST_DEV=joist-dev` goes back).

| | |
|---|---|
| Host | pvehome: Intel Core i5-14400, 16 threads, 15 GB, LVM-thin |
| VM | VMID **114**, `joist-dev2`; q35, `cpu host` (`/dev/kvm` works inside: the self-tests that need a real CPU's PCID, and the benchmark gate, run under it), 10 vCPUs, **affinity host CPUs 0, 1, 10-15** (one P-core and the E-cores: off VM 112's four P-cores and their sibling threads), 8 GB, 40 GB on `local-lvm`, `onboot 0` |
| OS | Debian 13.7 from `debian-13-genericcloud-amd64.qcow2`, checked against Debian's `SHA512SUMS` (SHA-512 begins `f46f0671a6e5bdec5291ab8972bae2f1`); cloud-init user `aditya` with the workstation's key |
| Address | DHCP on pvehome's LAN; reached as `ssh joist-dev2` by its link-local address, which follows from its MAC and doesn't change (`fe80::<link-local>%vmbr0`), jumping through `root@pvehome` (<pvehome-address>) |

Toolchain: the same versions from the same sources as VM 113's table
above, each verified the same way: clang and LLD 21.1.8 (apt.llvm.org, the
same key fingerprint), Limine 12.9.2 (`limine-binary.tar.xz`, the same
SHA-256), CBMC 6.11.0 and `tla2tools.jar` v1.7.4 (the SHA-256 values of
`docs/phase1-plan.md`), QEMU 10.0.13, xorriso 1.5.6, OpenJDK 21.0.12; and
Debian's `gcc` 14.2.0, which CBMC runs as its preprocessor (missing until
2026-10-13: `docs/phase3-plan.md`, *Correction*).

**It shares a host with VM 112**, and so do the host's own services
(containers among them). What that does to timings on VM 112, and what is
done about it (all found at Phase 3 steps 7 and 8, 2026-10-13):

- A virtual CPU of VM 112 losing its host CPU for about a millisecond while
  this VM was busy: this VM is now kept off VM 112's cores (the affinity
  above), and benchmarks are run with it idle.
- Proxmox pins VM 112 as a whole to its four CPUs; `tools/pve/boot.sh` now
  also puts each virtual CPU on one of them, in order.
- The CPU's clock moves between turbo steps (4.6 to 3.4 GHz seen) while
  the time stamp counter runs at a fixed rate, so the same work takes 30 %
  more or fewer counts from one run to the next. The benchmark measures in
  CPU cycles against a ruler of its own (`user/ipcbench`).
- For the first few tenths of a second after VM 112 starts, measurements
  come out up to half as slow again in some runs; the benchmark waits a
  second first.
- Short stretches when everything is a quarter slower; the benchmark
  reports the median of twenty segments, and the worst segment beside it.

## VM 112: the Joist kernel test VM

Created 2026-10-06. It boots Joist ISOs under Proxmox's own QEMU, on a real
Intel CPU through KVM, so tests run there on the hardware's behaviour rather
than QEMU's emulation (TCG), which is what the local tests use.

| | |
|---|---|
| Proxmox VMID | **112**, name `joist-kernel` |
| Host CPU | Intel Core i5-14400: 6 P-cores (host CPUs 0–11, two threads each) and 4 E-cores (12–15) |
| VM | q35, `cpu host`, **4 cores pinned to host CPUs 2, 4, 6, 8** (four distinct P-cores; none on an E-core, host CPU 0 left to Proxmox), 256 MB, no disk, no network, `onboot 0` |
| Firmware | set per run: SeaBIOS or OVMF (no EFI disk, so no Secure Boot keys) |
| Extra QEMU arguments | `-chardev file,id=joistlog,path=/root/joist-test/serial.log -device isa-serial,chardev=joistlog -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot` |

**How it's driven:** `tools/pve/boot.sh ISO bios|uefi TIMEOUT LOG` copies the
ISO to `local:iso/joist-test-112.iso`, sets the firmware, starts the VM under
a lock (`/root/joist-test/lock`, one run at a time), waits for it to stop,
and fetches the serial log. Proxmox doesn't pass on QEMU's exit status, so
the kernel prints `joist: finish code N` and the script rebuilds the status
from it, as `isa-debug-exit` would give it. `make pve-test`, `PVE=1 make
selftest` and `PVE=1 make fuzz-kernel` (in `kernel/`) use it.

The pinning is for the benchmarks (Phase 3): with a hybrid CPU, a vCPU that
migrates between a P-core and an E-core would make IPC timings bimodal.

# GNU/Hurd testbeds

| | VM 111 — `joist-hurd-amd64` | VM 110 — `joist-hurd` |
|---|---|---|
| Status | **live** | **destroyed 2026-09-24** |
| Architecture | **x86_64** — the target (§5.8) | i386 |
| Source | netinst install, ISO md5-verified | prebuilt image, build 20260314 |

## Access

```sh
ssh joist-amd64
```

The alias is in `~/.ssh/config` on the workstation. The VM sits on pvehome's
LAN (<lan-subnet>/24), which the office workstation (<lan-subnet>/24) cannot
reach directly, so it jumps through pvehome:

```
Host joist-amd64
    HostName <lan-address>
    User aditya
    ProxyJump root@<pvehome-address>
```

| | |
|---|---|
| Proxmox VMID | **111**, name `joist-hurd-amd64` |
| Guest IP | <lan-address> (DHCP lease — not reserved) |
| User | `aditya`, in `sudo` |
| Auth | the workstation's `id_ed25519.pub` is in `authorized_keys`; `sudo` needs the password |
| Console | Proxmox web UI (https://<pvehome-address>:8006) → 111 → Console |

## What it is

```
GNU debian-hurd 0.9 GNU-Mach 1.8+git20260805-up-amd64/Hurd-0.9 x86_64 GNU
Debian forky/sid · deb.debian.org/debian-ports sid + unreleased
4 GB RAM · 40 GB disk (37 GB root, ~34 GB free) · root device /dev/wd0s5
```

`/dev/wd0s5` is rumpdisk naming — storage goes through the userspace rump
driver, not an in-kernel one.

## VM configuration, and why

```
cores 2, memory 4096, cpu host, bios seabios, ostype other
sata0  local-lvm:vm-111-disk-0,cache=writeback,size=40G
sata1  none,media=cdrom          (installer ISO ejected after install)
net0   e1000=02:00:00:00:00:02,bridge=vmbr0
boot   order=sata0
```

**Both disk and CD-ROM must be SATA/AHCI, not IDE.** hurd-amd64 has no
in-kernel IDE driver. The trap is that SeaBIOS will happily *boot* an IDE
CD-ROM — BIOS INT13 needs no OS driver — and then the installer fails at
"Detect and mount installation media" with an error that points nowhere near
the real cause. Do not switch either device to virtio or IDE.

`onboot 0`: the VM does not start with the host. `qm start 111`.

## Tooling

Present: `rpctrace` (ships with the `hurd` package — the tool Phase 0 depends
on), `gcc`, `make`, `git`, `mig`, `dpkg-buildpackage`, `fakeroot`, `sudo`.

Installed 2026-09-30: **clang 21.1.8** (Debian `1:21.1.8-10`; VM 110 had `-13`),
`python3` 3.14.7, and the build-deps of `hello`, `glibc` and `hurd`. apt also
has `deb-src http://deb.debian.org/debian sid main` in
`/etc/apt/sources.list.d/debian-src.list`: debian-ports has no sources. The
build-deps did **not** move the system `hurd` package, which was already
0.9.git20260919-1, the version of the source trees.

Work directories: `~/trace` (rpctrace workloads) and `~/spike` (hurd, glibc
and gnumach sources, clang builds). Progress of the current run is in
`~/run-amd64.log`.

Note this was a netinst base system, far barer than VM 110's prebuilt image.
The installer set a root password rather than enabling sudo, so `sudo` had to
be installed from the console first — see *Gotchas*.

## Known limitations

1. **`nproc` reports 1** despite two assigned cores. GNU Mach is effectively
   uniprocessor, so nothing here parallelises and builds are slow: a full Hurd
   build took ~11 hours on the comparable i386 VM. Also worth noting against
   decision 0003, which assumes SMP from the design stage.
2. **DHCP, not static.** <lan-address> is a lease. Reserve it on the router
   before relying on the SSH alias.
3. **The Hurd console wedges on long injected keystroke lines.** Roughly 270
   characters mangled input on VM 110 and killed console input until a reset.
   Keep `qm monitor` + `sendkey` commands short; use SSH for real work.
4. **No backups.** The temporary snapshot `pre_trapcount_libc` (stage 15) was
   deleted on 2026-10-01; VM 111 has no snapshots. Also, `pbs-home` (CT 405) is stopped and unreachable, so nothing
   on this host is being backed up.

## Gotchas worth keeping

- **The Hurd does not build out-of-tree.** A separate build directory fails at
  once with `make[1]: *** libports: No such file or directory`. Copy the source
  tree and build in place.
- **Never pass `CFLAGS=` on the make command line.** `Makeconf` uses
  `CFLAGS +=`, and a command-line assignment overrides it wholesale, dropping
  `-std=gnu99` and breaking every compile. Append to `CC` instead:
  `make CC="gcc -Wtrampolines"`.
- **The Makefile's subdir lists use `\` continuations** — 19 libraries and 34
  programs, not the ~10 a first-line read suggests. `nfs` is in the source tree
  but not in `prog-subdirs`, so it never builds.
- **`su` cannot be driven by piping a password over SSH.** It calls `tcflush()`
  before prompting and discards anything sent early. Use the Proxmox console,
  where it gets a real tty.
- **`pkill -f "<pattern>"` over SSH can kill your own session** when the
  pattern appears in the remote command line you are running. Use `pgrep -x`.
  The same holds for `ps … | grep <pattern> | kill`: the remote `bash -c`
  carries the pattern too. It happened again on 2026-10-01 with
  `grep "…utils/rpctrace"`. Match on fields (`awk '$2 == "/hurd/ext2fs"'`) so
  the pattern can't match its own command line.
- **`pkill`/`pgrep` can hang outright** when a traced task is wedged: they
  query `proc` about every process. `ps -e -o pid,args` with a `timeout`, then
  `kill <pid>`, worked where `pkill -x rpctrace` hung.
- **A `ps` that times out prints nothing.** A watcher that reads "no output"
  as "process gone" will report a still-running process as finished (this
  happened once). Check `ps`'s exit status.
- **`bc` is not installed.** Use shell arithmetic on `date +%s`.
- **A tree-built rpctrace needs `-I /usr/share/msgids`.** Otherwise it looks
  under its configure prefix, prints msgid numbers, and the gsync patch never
  fires. The patched rpctrace is `~/spike/hurd-full-gcc/utils/rpctrace`; since
  2026-10-01 it carries both `rpctrace-gsync.patch` and
  `rpctrace-null-receive.patch` (sha256 `624567f6…`). The stage 14 traces were
  made with the gsync patch alone (`36da17aa…`).
- **Don't reuse a per-directory Hurd tree for a full build.** Dependency files
  from failed compiles name a bare `version.h`, and the top-level build then
  fails in seconds. Use a fresh copy (`tools/phase0/hurd-full.sh`).

## VM 110, retired

The i386 stopgap, used for all Phase 0 data collection because Debian publishes
a prebuilt bootable image only for `hurd-i386`. Destroyed on 2026-09-24 once the
trampoline cross-check finished. Its irreplaceable data is archived on pvehome:

| Archive | Contents |
|---|---|
| `/root/hurd-setup/archive/vm110-phase0.tgz` (11 MB) | the three `rpctrace` traces whose SHA-256 sums are the provenance for everything in `phase0-data/`, plus the spike logs, scripts and derived CSVs |
| `/root/hurd-setup/archive/vm110-trampolines.tgz` (123 KB) | `tramp-libs.log`, `tramp-progs.log`, `tramp.log` |

All three trace checksums were verified straight out of the tarball against
what `docs/phase0-data/README.md` records, before the VM was destroyed.

**The archives are on pvehome only, and pvehome has no working backup.** If
they matter, copy them somewhere else.

## Next steps

1. ~~Re-run the `rpctrace` workloads here, on x86_64.~~ Done 2026-09-30; see
   `phase0-data/README.md` stage 8 and decision 0010's amendment. Raw traces
   are in `~aditya/trace/`; don't delete them, since stage 8's checksums refer
   to them.
2. ~~Install clang~~ and ~~finish decision 0002's glibc build~~. Done
   2026-09-30: libc pass complete, libm pass stopped because of a clang
   frontend hang (`phase0-data/README.md` stage 9). Checked 2026-10-01: the hang
   is clang *on the Hurd* (stage 10). It does not reproduce on Linux, so build
   glibc on a Linux host, cross to the Hurd. clang 19 and 22 are now installed
   alongside 21 on VM 111.
4. ~~Clang builds of GNU Mach, the Hurd and MIG output~~. Done 2026-10-01; see
   `docs/clang-inventory.md`. Work trees on the VM: `~/spike/hurd-clang-21`,
   `~/spike/hurd-gcc` (in-tree copies), and GNU Mach's `build-clang-21` and
   `build-gcc`.
5. ~~GCC builds of the Hurd, GNU Mach and glibc from source~~. Done 2026-10-01,
   all clean (`phase0-data/README.md` stage 13). Full Hurd build:
   `~/spike/hurd-full-gcc`; glibc: `~/spike/glibc-2.43/build-gcc`.
3. ~~Replace `nested.py`~~. Done 2026-09-30: v2 finds all 80 trampolines and
   298 nested functions (was 252). See `phase0-data/README.md` stage 7.
