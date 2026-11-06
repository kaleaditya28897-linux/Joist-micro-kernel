#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# glibc-cross.sh - Phase 5 step 7, part 1: glibc 2.43 cross-built for the
# Hurd on x86-64 (x86_64-gnu) with clang on Linux, its Mach layer as it is:
# decision 0002's condition 1 (the first such build with clang), before any
# of sysdeps/joist goes in. The result is what fails, and why; the patches
# in tools/phase5/glibc-patches/ fix what clang can't build of the Hurd
# layer Joist keeps.
#
# usage: tools/phase5/glibc-cross.sh WORKDIR [JOBS]
#
# Joist's Mach layer (step 7 proper): glibc/sysdeps/joist is copied into
# the source as sysdeps/joist and glibc configured with --enable-joist
# (patch 0011), which puts it before the Mach layer's directories. JOIST=0
# builds part 1's glibc, the Mach layer alone.
#
# Inputs, each checked by SHA-256: glibc's source as GNU releases it (its
# signature checked once, 2026-11-05: good, from release manager Andreas K.
# Huettel, RSA FD19E6D31B192EE4DC63EAD3DC2B16215ED5412A; the same files as
# Debian's glibc_2.43.orig.tar.xz that tools/phase5/glibc-inventory.sh
# reads, and manual/, which Debian drops and the Hurd's errno list is made
# from); the Hurd's and Mach's headers from Debian's hurd-amd64 packages;
# MIG built from third_party/mig. Nothing downloaded is run.
#
# One stand-in, recorded for 0002: upstream clang lacks __float128 for the
# Hurd target (docs/clang-inventory.md; Debian patches it in), so the
# compiler is clang's Linux x86-64 target with the Hurd's predefined macros.
# The objects and the calling convention are the same.
set -eu
W=$(mkdir -p "$1" && cd "$1" && pwd)
J=${2:-2}
JOIST=${JOIST:-1}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
PORTS=https://deb.debian.org/debian-ports
cd "$W"
mkdir -p debs headers bin mig/bin mig/libexec logs

get() { # URL SHA256 FILE
    [ -f "$3" ] || curl -sSf -o "$3" "$1"
    echo "$2  $3" | sha256sum -c --quiet
}

get $PORTS/pool-hurd-amd64/main/h/hurd/hurd-dev_0.9.git20261028-4_hurd-amd64.deb \
    12d304ca12c194fa1c073bc2cdde996ea14bf8f409e52a1530c976a375d6093d debs/hurd-dev.deb
get $PORTS/pool-hurd-amd64/main/g/gnumach/gnumach-dev_1.8+git20261028-1_hurd-amd64.deb \
    803a7cf4ebfe8a5e4d97c31caab842bad6213526a98610e1b0e0a76845a2bc61 debs/gnumach-dev.deb
get https://ftp.gnu.org/gnu/glibc/glibc-2.43.tar.xz \
    d9c86c6b5dbddb43a3e08270c5844fc5177d19442cf5b8df4be7c07cd5fa3831 glibc-2.43.tar.xz

# The kernel's and the Hurd's headers only (not an installed glibc's), in
# one include directory, as --with-headers wants them.
if [ ! -f headers/.done ]; then
    rm -rf headers/root headers/include && mkdir -p headers/root headers/include
    for d in debs/hurd-dev.deb debs/gnumach-dev.deb; do
        rm -rf x && mkdir x && (cd x && ar x "../$d" && tar -xf data.tar.* -C ../headers/root)
    done
    rm -rf x
    cp -r headers/root/usr/include/. headers/include/
    if [ -d headers/include/x86_64-gnu ]; then
        cp -r headers/include/x86_64-gnu/. headers/include/
        rm -rf headers/include/x86_64-gnu
    fi
    touch headers/.done
fi
# glibc's source, fresh each run, with Joist's patches (tools/phase5/
# glibc-patches/, in order): what clang needs of the Hurd layer Joist keeps.
rm -rf src && mkdir -p src && tar -xJf glibc-2.43.tar.xz -C src
for p in "$here"/glibc-patches/*.patch; do
    [ -f "$p" ] || continue
    (cd src/glibc-2.43 && patch -p1 --quiet --no-backup-if-mismatch < "$p") ||
        { echo "patch failed: $p"; exit 1; }
done
joist_opt=
if [ "$JOIST" = 1 ]; then
    cp -r "$repo/glibc/sysdeps/joist" src/glibc-2.43/sysdeps/joist
    joist_opt=--enable-joist
fi

# The compiler: clang for x86_64-gnu (see above), linking with lld.
cat > bin/x86_64-gnu-clang <<'EOF'
#!/bin/sh
exec clang --target=x86_64-unknown-linux-gnu -U__linux__ -U__linux -Ulinux -U__gnu_linux__ \
    -D__GNU__=1 -D__gnu_hurd__=1 -D__MACH__=1 -fuse-ld=lld -Wno-unused-command-line-argument "$@"
EOF
chmod +x bin/x86_64-gnu-clang

# MIG: migcom from third_party/mig, and its driver from mig.in. For Joist
# (JOIST=1), joistmig in migcom's place: the same driver, the same
# options (-i for glibc's one file a routine), stubs that speak Joist's IPC
# through glibc's sysdeps/joist/joist_mig.h.
if [ "$JOIST" = 1 ]; then migmain=joistmig.c; else migmain=migcom.c; fi
(cd "$repo/third_party/mig" && clang -O2 -w -I. -o "$W/mig/libexec/x86_64-gnu-migcom" error.c global.c \
    header.c lexxer.c $migmain parser.c routine.c server.c statement.c string.c type.c user.c utils.c)
sed -e "s#@prefix@#$W/mig#" -e 's#@exec_prefix@#${prefix}#' -e 's#@bindir@#${prefix}/bin#' \
    -e 's#@libexecdir@#${prefix}/libexec#' -e 's#@MIGCOM@#x86_64-gnu-migcom#' \
    -e "s#@CC@#$W/bin/x86_64-gnu-clang#" -e "s#@TARGET_CC@#$W/bin/x86_64-gnu-clang#" \
    -e 's#@MIG@#x86_64-gnu-mig#' -e 's#@PACKAGE_NAME@#GNU MIG#' -e 's#@PACKAGE_VERSION@#1.8+git20231217#' \
    "$repo/third_party/mig/mig.in" > mig/bin/x86_64-gnu-mig
chmod +x mig/bin/x86_64-gnu-mig

rm -rf build && mkdir build
cd build
echo "configure: $(date -u)" > "$W/logs/stamps"
"$W/src/glibc-2.43/configure" --host=x86_64-gnu --build=x86_64-pc-linux-gnu --prefix=/usr \
    --with-headers="$W/headers/include" --disable-werror --disable-profile --disable-nscd \
    --disable-timezone-tools $joist_opt CC="$W/bin/x86_64-gnu-clang" CXX=false BUILD_CC=clang \
    AR=llvm-ar RANLIB=llvm-ranlib NM=llvm-nm OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump \
    READELF=llvm-readelf MIG="$W/mig/bin/x86_64-gnu-mig" > "$W/logs/configure.log" 2>&1 ||
    { echo "configure failed: logs/configure.log"; tail -20 "$W/logs/configure.log"; exit 1; }
echo "make: $(date -u)" >> "$W/logs/stamps"
# -k: past the first failure, so the log holds every one.
make -k -j"$J" > "$W/logs/make.log" 2>&1 && st=0 || st=$?
echo "done (make $st): $(date -u)" >> "$W/logs/stamps"
grep -c "error:" "$W/logs/make.log" || true
exit "$st"
