#!/bin/sh
# stage.sh - one step of the DDC inside live-bootstrap's final system
# (decision 0026, layer 3). tools/ddc/ddc.sh runs it there through
# guest.sh, with its inputs in /in and the work disk at /work.
#
# usage: stage.sh tools     cmake and ninja, from source, by the bootstrapped
#                           GCC, into /work/tools
#        stage.sh stage1T   path T's stage 1: LLVM by the bootstrapped GCC,
#                           into /work/t/stage1
#        stage.sh stage2 NAME  a stage 2 by the stage 1 in /work/NAME/stage1
#                           (t or d), into /work/NAME/stage2; with
#                           KEEP_BUILD=1, its objects into /work/NAME/build
#
# Every stage 2 is built the same way whatever its stage 1: the stage 1 is
# copied to /work/stage1 first, the build is in /work/build and installs
# into /work/stage2, and only then is the result moved to /work/NAME.
set -eu
in=/in
jobs=6
export PATH=/work/tools/bin:/usr/bin:/usr/sbin
export SOURCE_DATE_EPOCH=1765883392 LC_ALL=C TZ=UTC
umask 022
src=/work/src/llvm-project-21.1.8.src
# Where a stage 1 Clang finds the system's C++ library, C library and GCC
# runtime: live-bootstrap's layout is not one Clang searches by itself
# (--gcc-install-dir is not enough). The same for every stage 2.
t=i686-unknown-linux-musl
lib=/usr/lib/$t
gcclib=$lib/gcc/$t/15.2.0
cxx=/usr/include/c++/15.2.0
stage2_flags="-stdlib++-isystem $cxx -stdlib++-isystem $cxx/$t -stdlib++-isystem $cxx/backward
    -B$gcclib -B$lib -L$gcclib -L$lib -Wno-unused-command-line-argument"

unpack_llvm() {
    [ -d "$src" ] && return
    mkdir -p /work/src
    echo "4633a23617fa31a3ea51242586ea7fb1da7140e426bd62fc164261fe036aa142  $in/llvm-project-21.1.8.src.tar.xz" |
        sha256sum -c
    tar -C /work/src -xJf "$in/llvm-project-21.1.8.src.tar.xz"
}

case $1 in
tools)
    sha256sum -c <<EOF
653427f0f5014750aafff22727fb2aa60c6c732ca91808cfb78ce22ddd9e55f0  $in/cmake-3.31.6.tar.gz
821bdff48a3f683bc4bb3b6f0b5fe7b2d647cf65d52aeb63328c91a6c6df285a  $in/ninja-1.12.1.tar.gz
EOF
    rm -rf /work/tools /work/tools-build
    mkdir -p /work/tools-build
    cd /work/tools-build
    tar xzf "$in/ninja-1.12.1.tar.gz"
    (cd ninja-1.12.1 && python3 configure.py --bootstrap)
    mkdir -p /work/tools/bin
    cp ninja-1.12.1/ninja /work/tools/bin/
    tar xzf "$in/cmake-3.31.6.tar.gz"
    # No network code: none is needed, and it would need OpenSSL.
    (cd cmake-3.31.6 && ./bootstrap --prefix=/work/tools --parallel=$jobs \
        -- -DCMAKE_USE_OPENSSL=OFF -DBUILD_TESTING=OFF &&
        make -j$jobs && make install)
    cd /work
    rm -rf /work/tools-build
    cmake --version
    ninja --version
    ;;
stage1T)
    unpack_llvm
    gcc --version
    JOBS=$jobs sh "$in/build-llvm.sh" gcc g++ "$src" /work/build /work/t/stage1
    rm -rf /work/build
    ;;
stage2)
    name=$2
    unpack_llvm
    [ -x "/work/$name/stage1/bin/clang" ] || { echo "stage: no /work/$name/stage1"; exit 1; }
    rm -rf /work/stage1 /work/stage2 "/work/$name/stage2"
    cp -a "/work/$name/stage1" /work/stage1
    /work/stage1/bin/clang --version
    JOBS=$jobs LINKER=lld EXTRA_FLAGS=$(echo $stage2_flags) \
        sh "$in/build-llvm.sh" /work/stage1/bin/clang /work/stage1/bin/clang++ \
        "$src" /work/build /work/stage2
    mv /work/stage2 "/work/$name/stage2"
    # KEEP_BUILD=1 keeps the objects, to find which differ (findings).
    rm -rf "/work/$name/build"
    if [ "${KEEP_BUILD:-0}" = 1 ]; then mv /work/build "/work/$name/build"; fi
    rm -rf /work/stage1 /work/build
    ;;
*)
    echo "usage: stage.sh tools | stage1T | stage2 NAME"; exit 2 ;;
esac
