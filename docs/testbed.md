# GNU/Hurd testbed

A persistent Debian GNU/Hurd VM on **pvehome**, for the Phase 0 inventory work
(§11) and as a reference system for the rest of the project.

## Access

```sh
ssh -J root@<pvehome-address> demo@<lan-address>
```

The VM sits on pvehome's LAN (<lan-subnet>/24), which is **not reachable from
the office workstation** (<lan-subnet>/24), so the jump through pvehome is
required. Add this to `~/.ssh/config` to shorten it:

```
Host joist-hurd
    HostName <lan-address>
    User demo
    ProxyJump root@<pvehome-address>
```

Then `ssh joist-hurd`.

| | |
|---|---|
| Proxmox VMID | **110**, name `joist-hurd` |
| Host | pvehome (<pvehome-address>) |
| Guest IP | <lan-address> (DHCP — see caveat below) |
| Users | `demo` (password `joisthurd2026`, in `sudo`), `root` (no password, console only) |
| Auth | The workstation's `id_ed25519.pub` is in `demo`'s `authorized_keys`. |
| Console | Proxmox web UI → VM 110 → Console, or `qm terminal 110` on the host. |

## What it is

Debian GNU/Hurd **i386**, build 20260314, from the prebuilt image at
`cdimage.debian.org/cdimage/ports/latest/hurd-i386/`.

```
GNU debian 0.9 GNU-Mach 1.8+git20260224-up-486/Hurd-0.9 i686-AT386 GNU
Debian forky/sid, apt sources: deb.debian.org/debian-ports sid + unreleased
```

**It is i386, not x86_64**, although the plan targets x86_64 first (§5.8).
Debian publishes a prebuilt, immediately bootable image only for `hurd-i386`;
`hurd-amd64` ships a netinst ISO that needs an interactive install. For Phase 0
— `rpctrace` on real workloads, reading the Mach `.defs`, inventorying what the
Hurd actually uses — the architecture does not matter. It will matter when we
start drawing userland for the x86_64 target, at which point a second VM should
be built from `debian-sid-hurd-amd64-NETINST-1.iso`.

## VM configuration

```
cores 2, memory 2048, cpu host, bios seabios, ostype other
ide0  local-lvm:vm-110-disk-0,cache=writeback,size=24480M
net0  e1000=02:00:00:00:00:01,bridge=vmbr0
serial0 socket, vga std, onboot 0
```

`ide0` and `e1000` are deliberate: the Hurd's driver support is narrow, and
these are what the upstream image is tested against. Do not switch to virtio.

`onboot 0` — the VM does not start with the host, to avoid holding 2 GB on a
16 GB machine permanently. Start it with `qm start 110`, or set `onboot 1` if
you would rather have it always available.

## Tooling state

Present in the image: `rpctrace` (the tool Phase 0 depends on), `gcc`, `make`,
`fakeroot`, `dpkg-buildpackage`.

Installed afterwards: `git`, `mig`.

Installed 2026-09-23 for decision 0002's spike: **clang 21.1.8** (`clang`,
from debian-ports), plus the build dependencies of `glibc` and `hurd`.
**`lld` is not available** for hurd-i386.

**Package drift:** `apt-get build-dep glibc hurd` upgraded the system `hurd`
package from `0.9.git20251029-7` to `0.9.git20260919-1`. dpkg stopped at a
conffile prompt for `hurd-console`; it was finished with
`dpkg --force-confold --configure -a`, which keeps the local file. The new Hurd
runs from the next reboot. The Phase 0 rpctrace data was collected under the
old version. Note this before comparing traces taken after a reboot.

apt also has `deb-src http://deb.debian.org/debian sid main`
(`/etc/apt/sources.list.d/debian-src.list`). debian-ports publishes no
sources, so `apt-get source` / `build-dep` need the main archive.

## Known limitations

1. **Disk expanded 2026-09-23** from 4 GB to 24 GB; `/` (`hd0s2`) is 23 GB
   with ~20 GB free. Done offline from the host, not from inside the Hurd,
   whose ext2fs cannot resize online:
   ```sh
   # in the guest: sudo poweroff ; then on pvehome:
   qm snapshot 110 pre_disk_resize
   qm resize 110 ide0 +20G
   echo ", +" | sfdisk --no-reread -N 2 /dev/pve/vm-110-disk-0
   L=$(losetup -f --show -P /dev/pve/vm-110-disk-0)
   e2fsck -f -y ${L}p2 ; resize2fs ${L}p2 ; e2fsck -f -n ${L}p2
   losetup -d $L ; qm start 110
   ```
   The first `e2fsck` fixed 13 deleted inodes with zero dtime, plus the stale
   free counts that went with them. These are leftovers of a Hurd shutdown, not
   corruption; the second check came back clean. The old partition table is in
   `/root/vm110-ptable-before-resize.sfdisk` on pvehome. The Proxmox snapshot
   `pre_disk_resize` still exists; delete it (`qm delsnapshot 110
   pre_disk_resize`) once the VM has proven healthy, because it pins the old
   blocks in the thin pool.
   *Host caveat:* pvehome's thin pool is overcommitted (~520 GiB of thin
   volumes on a 338 GiB pool, 66% actually used). That's fine today, but watch
   `lvs` before growing anything else.
2. **DHCP, not static.** <lan-address> is a lease and may change. Either
   reserve it on the router or give the VM a static address before relying on
   the SSH config entry above.
3. **Console input is fragile.** Driving the VGA console by injecting
   keystrokes (`qm monitor` → `sendkey`) works for short commands but wedges on
   long ones — a ~270-character line mangled its input and stopped responding
   until a reset. Use SSH for anything non-trivial. This is also a small
   data point about the Hurd console that may be worth remembering.
4. **Image checksum unverified.** The `MD5SUMS` published alongside the
   `debian-hurd.img.tar.xz` symlink covers older builds (20230608, 20250807),
   not the 20260314 image the symlink actually serves. The download was HTTPS
   from cdimage.debian.org with a byte-exact content length and a valid
   partition table, but no checksum match was possible.

## Next steps on this VM

Phase 0 (§11), in rough order:

1. ~~`rpctrace` on boot, a shell session, and a package build → `docs/mach-inventory.md`.~~
   Done 2026-09-23, partially: rpctrace deadlocks on `gsync_wait`. See the
   inventory's *Coverage* section. Scripts are in `tools/phase0/`, data and
   provenance in `docs/phase0-data/`.
   Raw traces are in `~demo/trace/`; don't delete them, since the checksums in
   `docs/phase0-data/README.md` refer to them.
2. ~~Read `/usr/include/mach/*.defs` and mark every call used / unused / used-by-one-thing.~~
   Done by grep cross-reference (inventory *Static results*); reading the call
   sites behind the known-hard rows is still open.
3. ~~Answer the open questions in `docs/client-recovery.md` §8~~ (done; Q2 is
   partial) — especially
   whether ext2fs exposes an inode generation counter, and what a Hurd file
   descriptor actually holds.
4. ~~Expand the disk~~, ~~install clang~~ (done); decision 0002's sizing spike is
   **running**: `~demo/spike/spike.sh`, progress in `~demo/spike/spike.log`,
   glibc build log in `~demo/spike/glibc-make.log` (capped at 5 h). Sources
   and clang build trees are in `~demo/spike/`.
