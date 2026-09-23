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
ide0  local-lvm:vm-110-disk-0,cache=writeback,size=4000M
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

Not present: **`clang`**. Decision 0002's sizing spike needs it, and installing
LLVM will not fit in the current disk (see below).

## Known limitations

1. **Disk is small.** 4 GB total, ~1.5 GB free. Not enough to build glibc or
   the Hurd from source, and not enough for LLVM. Expanding means
   `qm resize 110 ide0 +20G` on the host *and* growing the ext2 partition from
   inside the Hurd, which is the fiddly half. Do this before Phase 0's build
   work starts.
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

1. `rpctrace` on boot, a shell session, and a package build → `docs/mach-inventory.md`.
2. Read `/usr/include/mach/*.defs` and mark every call used / unused / used-by-one-thing.
3. Answer the open questions in `docs/client-recovery.md` §8 — especially
   whether ext2fs exposes an inode generation counter, and what a Hurd file
   descriptor actually holds.
4. Expand the disk, install clang, and run decision 0002's sizing spike.
