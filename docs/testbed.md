# GNU/Hurd testbeds

The project runs one live testbed on **pvehome**, with a second now retired.

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
4. **No backups.** `pbs-home` (CT 405) is stopped and unreachable, so nothing
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
- **`pkill`/`pgrep` can hang outright** when a traced task is wedged: they
  query `proc` about every process. `ps -e -o pid,args` with a `timeout`, then
  `kill <pid>`, worked where `pkill -x rpctrace` hung.
- **A `ps` that times out prints nothing.** A watcher that reads "no output"
  as "process gone" will report a still-running process as finished (this
  happened once). Check `ps`'s exit status.
- **`bc` is not installed.** Use shell arithmetic on `date +%s`.
- **A tree-built rpctrace needs `-I /usr/share/msgids`.** Otherwise it looks
  under its configure prefix, prints msgid numbers, and the gsync patch never
  fires. The patched rpctrace is `~/spike/hurd-full-gcc/utils/rpctrace`.
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
