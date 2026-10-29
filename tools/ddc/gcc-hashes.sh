#!/bin/sh
# gcc-hashes.sh - the sha256 of the bootstrapped GCC and what it uses
# (decision 0026, layer 2; docs/toolchain/record.md), read straight out of
# live-bootstrap's final disk with debugfs, without booting it.
#
# usage: tools/ddc/gcc-hashes.sh IMAGE   (target/external.img, or its copy)
set -eu
image=$1
PATH=$PATH:/usr/sbin:/sbin
tmp=$(mktemp -d "${TMPDIR:-/tmp}/gcc-hashes.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
# The root file system starts 1 MiB into the disk (lib/utils.py).
root="$image?offset=1048576"
t=i686-unknown-linux-musl
lib=/usr/lib/$t
gcclib=$lib/gcc/$t/15.2.0
libexec=/usr/libexec/gcc/$t/15.2.0

for f in /usr/bin/gcc /usr/bin/g++ /usr/bin/cpp \
    $libexec/cc1 $libexec/cc1plus $libexec/collect2 $libexec/lto-wrapper \
    /usr/bin/as /usr/bin/ld /usr/bin/ld.bfd /usr/bin/ar /usr/bin/ranlib \
    $lib/libstdc++.a $lib/libstdc++.so.6.0.34 $lib/libgcc_s.so.1 \
    $gcclib/libgcc.a $gcclib/libgcc_eh.a $gcclib/crtbegin.o $gcclib/crtend.o \
    $lib/libc.a $lib/libc.so $lib/crt1.o $lib/crti.o $lib/crtn.o \
    /boot/vmlinuz; do
    # A symbolic link is shown as such, with its target, not followed.
    if debugfs -R "stat $f" "$root" 2>/dev/null | grep -q 'Type: symlink'; then
        echo "symlink  $f -> $(debugfs -R "stat $f" "$root" 2>/dev/null |
            sed -n 's/^Fast link dest: "\(.*\)"$/\1/p')"
        continue
    fi
    rm -f "$tmp/f"
    debugfs -R "dump $f $tmp/f" "$root" 2>/dev/null
    if [ -s "$tmp/f" ]; then
        echo "$(sha256sum < "$tmp/f" | cut -d' ' -f1)  $f"
    else
        echo "missing  $f"
    fi
done
