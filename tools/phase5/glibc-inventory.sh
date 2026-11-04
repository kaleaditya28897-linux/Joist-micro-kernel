#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# glibc-inventory.sh - Phase 5 step 5, decision 0031's check: what libports,
# libihash, libtrivfs, trans/hello, a test client and a raw libports server
# reach in glibc, and what of that stands on Mach.
#
# usage: tools/phase5/glibc-inventory.sh WORKDIR
#
# Debian's hurd-amd64 binaries (glibc's libc.a, the Hurd's static libraries,
# headers, libgcc) and the Hurd's and glibc's sources, each checked against
# its SHA-256; the programs compiled with clang and linked statically with
# ld.lld, which records why every archive member is pulled in
# (--why-extract); tools/phase5/glibc-inventory.py classifies them.
# Nothing downloaded is run: the only programs run are this machine's.
#
# Two stand-ins, neither of which changes what is linked:
# - clang's Linux x86-64 target with the Hurd's macros, since upstream clang
#   lacks __float128 for the Hurd target (docs/clang-inventory.md); the ELF
#   objects and calling convention are the same;
# - an empty trivfs_io_S.h (MIG's server header: prototypes of the routines
#   hello.c defines itself; the server code is in libtrivfs.a already).
set -eu
W=$(mkdir -p "$1" && cd "$1" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
PORTS=https://deb.debian.org/debian-ports
MAIN=https://deb.debian.org/debian
cd "$W"
mkdir -p debs sysroot gen/libtrivfs obj out

get() { # URL SHA256 FILE
    [ -f "$3" ] || curl -sSf -o "$3" "$1"
    echo "$2  $3" | sha256sum -c --quiet
}

get $PORTS/pool-hurd-amd64/main/g/glibc/libc0.3-dev_2.43-7_hurd-amd64.deb \
    6be6dd60aff4ff99776b45d4ae4b4eeafe8f0539ba12c6386d85d152e34a456b debs/libc0.3-dev.deb
get $PORTS/pool-hurd-amd64/main/g/glibc/libc0.3_2.43-7_hurd-amd64.deb \
    d014aab99750f218b41e5e9d9dfe836cda307f57728055b0c0a1575caeefad65 debs/libc0.3.deb
get $PORTS/pool-hurd-amd64/main/h/hurd/hurd-dev_0.9.git20261028-4_hurd-amd64.deb \
    12d304ca12c194fa1c073bc2cdde996ea14bf8f409e52a1530c976a375d6093d debs/hurd-dev.deb
get $PORTS/pool-hurd-amd64/main/g/gnumach/gnumach-dev_1.8+git20261028-1_hurd-amd64.deb \
    803a7cf4ebfe8a5e4d97c31caab842bad6213526a98610e1b0e0a76845a2bc61 debs/gnumach-dev.deb
get $PORTS/pool-hurd-amd64/main/g/gcc-15/libgcc-15-dev_15.3.0-5_hurd-amd64.deb \
    3cf2ec1bc99f80f900892074513e457b44ba33f14d8271a308e9145d38ad250f debs/libgcc-15-dev.deb
get $MAIN/pool/main/h/hurd/hurd_0.9.git20261028.orig.tar.bz2 \
    2a17feadadf1d1cd632c446bac259acedd1c1e1bcea786c0d2118bf9d008bd27 hurd.orig.tar.bz2
get $MAIN/pool/main/g/glibc/glibc_2.43.orig.tar.xz \
    b8f0d50a7d4255658efe1f608cacdc4be82bb3c009875626df5968cc64c58880 glibc.orig.tar.xz

for d in debs/*.deb; do
    rm -rf x && mkdir x && (cd x && ar x "../$d" && tar -xf data.tar.* -C ../sysroot)
done
rm -rf x
[ -d src ] || { mkdir src && tar -xjf hurd.orig.tar.bz2 -C src; }
tar -tJf glibc.orig.tar.xz > glibc.files
S=$W/src/hurd-0.9.git20261028
R=$W/sysroot
L=$R/usr/lib/x86_64-gnu
G=$R/usr/lib/gcc/x86_64-gnu/15

sed -e 's/MASTER_HURD_VERSION/"0.9"/' "$S/version.h.in" > gen/version.h
printf '/* stand-in for MIG'"'"'s trivfs_io_S.h: see glibc-inventory.sh */\n' > gen/libtrivfs/trivfs_io_S.h

cc() {
    clang --target=x86_64-unknown-linux-gnu --sysroot="$R" -nostdinc \
        -isystem "$(clang -print-resource-dir)/include" -isystem "$R/usr/include/x86_64-gnu" \
        -isystem "$R/usr/include" -U__linux__ -U__linux -Ulinux -U__gnu_linux__ \
        -D__GNU__=1 -D__gnu_hurd__=1 -D__MACH__=1 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
        -std=gnu99 -O2 -fno-stack-protector "$@"
}

link() { # OUT OBJ: first without an executable stack, to list the members
         # that need one (GCC nested functions), then with it
    libs="$L/libtrivfs.a $L/libports.a $L/libiohelp.a $L/libfshelp.a $L/libihash.a
          $L/libshouldbeinlibc.a $L/libpthread.a $L/libhurduser.a $L/libmachuser.a $L/libc.a
          $G/libgcc.a $G/libgcc_eh.a"
    # shellcheck disable=SC2086
    ld.lld -static -m elf_x86_64 -L"$L" -o "$1" -e _start --why-extract="$1.why" \
        "$L/crt1.o" "$L/crti.o" "$G/crtbeginT.o" "$2" --start-group $libs --end-group \
        "$G/crtend.o" "$L/crtn.o" 2>&1 |
        sed -n 's#.*/\([^/]*\.a([^)]*)\): requires an executable stack.*#\1#p' | sort -u > "$1.execstack" || true
    # shellcheck disable=SC2086
    ld.lld -static -m elf_x86_64 -z execstack -L"$L" -o "$1" -e _start --why-extract="$1.why" \
        "$L/crt1.o" "$L/crti.o" "$G/crtbeginT.o" "$2" --start-group $libs --end-group \
        "$G/crtend.o" "$L/crtn.o"
}

cc -I"$W/gen" -I"$S" -c "$S/trans/hello.c" -o obj/hello.o
cc -c "$here/client.c" -o obj/client.o
cc -c "$here/portsrv.c" -o obj/portsrv.o
for p in hello client portsrv; do
    link "out/$p" "obj/$p.o"
done

python3 -I "$here/glibc-inventory.py" "$W/result" "$R" "$W/glibc.files" \
    out/hello.why out/client.why out/portsrv.why
D=$repo/docs/phase5-data/step5-glibc-inventory
cp result/members.csv result/mach-interface.csv "$D/"
sort -u out/*.execstack > "$D/nested-functions.txt"
for p in hello client portsrv; do
    cut -f2 "out/$p.why" | tail -n +2 | sed 's#.*/##' | sort -u > "$D/$p-members.txt"
done
echo "inventory: $D"
