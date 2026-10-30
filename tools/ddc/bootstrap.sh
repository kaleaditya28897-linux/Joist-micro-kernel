#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# bootstrap.sh - build the trusted GCC from live-bootstrap's hex0 seed
# (decision 0026, layer 2; docs/toolchain/record.md).
#
# Fetches live-bootstrap at the pinned commit with its submodules, checks
# every commit and the seed's hash against the record, fetches the source
# tarballs into DISTFILES (live-bootstrap checks each one's sha256 against
# its own list), then runs the whole bootstrap in a QEMU guest with KVM.
# At the end WORK/target/external.img is the final disk: an i386 musl system
# with GCC 15.2.0; tools/ddc/gcc-hashes.sh then hashes the GCC in it.
#
# usage: tools/ddc/bootstrap.sh WORK   (on the build VM; several hours)
# Environment: DISTFILES (default WORK/distfiles), CORES (default 6),
# RAM in MiB (default 6144).
# Needs: git, python3 with requests, qemu-system-x86_64, parted, mkfs.ext3.
set -eu
work=$1
distfiles=${DISTFILES:-$work/distfiles}
cores=${CORES:-6}
ram=${RAM:-6144}
# parted and mkfs.ext3 live in sbin, which a login shell may not have.
PATH=$PATH:/usr/sbin:/sbin

# The pins (record.md): live-bootstrap has no tagged release, so a commit.
lb_url=https://github.com/fosslinux/live-bootstrap
lb_commit=b1ceced7ea8a819a26f23796f6dd7ae8496a33a4
pins="builder-hex0 a2781242d19e6be891b453d8fa827137ab5db31a
seed/stage0-posix 45d90f5955b6907dc6cdea9ebafce558359edcd3
seed/stage0-posix/x86 3b9c2bb6d4155e4f2e5f642b5e0f59255dfc5934
seed/stage0-posix/M2libc 68a23cfd05d5a355ba7a30c770d684cbe86fcc4e
seed/stage0-posix/M2-Planet bd2fe4b0659fd0ad3f476a5ad0ef801bd134665d
seed/stage0-posix/mescc-tools 5adfbf3364261a77109878a56b100aeeb6ef9ac4
seed/stage0-posix/mescc-tools-extra a151c245e512076971a3c85bb1502cf92cfa83b6"
# The only binary in the chain in QEMU mode: builder-hex0's boot sector, as
# rootfs.py makes it from builder-hex0-x86-stage1.hex0 (seed-review.md).
seed_sha256=87c3d44555bd294dbc7be12dd06697235a354247c155eb0eb5a8834a3aad1b59

lb=$work/live-bootstrap
mkdir -p "$work" "$distfiles"
[ -d "$lb/.git" ] || git clone -q "$lb_url" "$lb"
cd "$lb"
git checkout -q "$lb_commit"
git submodule update -q --init --recursive
[ "$(git rev-parse HEAD)" = "$lb_commit" ] ||
    { echo "bootstrap: live-bootstrap is not at $lb_commit"; exit 1; }
echo "$pins" | while read -r path commit; do
    [ "$(git -C "$path" rev-parse HEAD)" = "$commit" ] ||
        { echo "bootstrap: $path is not at $commit"; exit 1; }
done
[ -z "$(git status --porcelain --ignore-submodules=none | grep -v '^?? distfiles$')" ] ||
    { echo "bootstrap: the live-bootstrap tree has local changes"; exit 1; }

# The seed, made the way generator.py makes it.
python3 -I -c '
import sys
out = open(sys.argv[2], "wb")
for line in open(sys.argv[1]):
    out.write(bytes.fromhex(line.split("#")[0].split(";")[0].strip()))
' builder-hex0/builder-hex0-x86-stage1.hex0 "$work/seed-stage1.bin"
echo "$seed_sha256  $work/seed-stage1.bin" | sha256sum -c --quiet ||
    { echo "bootstrap: the seed is not the one in the record"; exit 1; }

# The sources: fetched here, checked against live-bootstrap's own list.
# mirror.sh, not download-distfiles.sh: some sources have no tarball to
# download, only a git commit, and mirror.sh makes them with git archive.
ln -sfn "$distfiles" distfiles
./mirror.sh "$distfiles" "$work/mirrorstate"

# The bootstrap itself. It powers the guest off at the end.
rm -rf "$work/target"
start=$(date +%s)
# rootfs.py wants a mirror; the local one is the distfiles just checked.
python3 rootfs.py --qemu --external-sources --cores "$cores" --qemu-ram "$ram" \
    --mirrors "file://$distfiles" --target "$work/target"
echo "bootstrap: finished in $(( $(date +%s) - start )) s"
sha256sum "$work/seed-stage1.bin"
