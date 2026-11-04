#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# run.sh - MIG's Joist mode, tested on the host (Phase 5 step 6).
#
# usage: tools/mig-test/run.sh WORKDIR [test|fuzz|mutate]
#   test     (the default) every carried routine of io, fs and fsys round
#            trips through a fake transport, SEEDS seeds (default 1000), at
#            -O0, -O1 and -O2, under ASan and UBSan
#   fuzz     libFuzzer on the three demuxers, FUZZ_TIME seconds (default 60)
#   mutate   each mutation in tools/mig-test/mutations.txt must make the
#            round trip, or a FUZZ_TIME-second fuzz (default 20), fail
#
# The Hurd's and Mach's headers come from Debian's hurd-amd64 packages, each
# checked by SHA-256 (as tools/phase5/glibc-inventory.sh); nothing downloaded
# is run. joistmig is built from third_party/mig.
set -eu
W=$(mkdir -p "$1" && cd "$1" && pwd)
mode=${2:-test}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
PORTS=https://deb.debian.org/debian-ports
SEEDS=${SEEDS:-1000}

get() { # URL SHA256 FILE
    [ -f "$3" ] || curl -sSf -o "$3" "$1"
    echo "$2  $3" | sha256sum -c --quiet
}

sysroot() {
    mkdir -p "$W/debs" "$W/sysroot"
    get $PORTS/pool-hurd-amd64/main/g/glibc/libc0.3-dev_2.43-7_hurd-amd64.deb \
        6be6dd60aff4ff99776b45d4ae4b4eeafe8f0539ba12c6386d85d152e34a456b "$W/debs/libc0.3-dev.deb"
    get $PORTS/pool-hurd-amd64/main/h/hurd/hurd-dev_0.9.git20261028-4_hurd-amd64.deb \
        12d304ca12c194fa1c073bc2cdde996ea14bf8f409e52a1530c976a375d6093d "$W/debs/hurd-dev.deb"
    get $PORTS/pool-hurd-amd64/main/g/gnumach/gnumach-dev_1.8+git20261028-1_hurd-amd64.deb \
        803a7cf4ebfe8a5e4d97c31caab842bad6213526a98610e1b0e0a76845a2bc61 "$W/debs/gnumach-dev.deb"
    if [ ! -f "$W/sysroot/.done" ]; then
        for d in "$W"/debs/*.deb; do
            rm -rf "$W/x" && mkdir "$W/x" && (cd "$W/x" && ar x "$d" && tar -xf data.tar.* -C "$W/sysroot")
        done
        rm -rf "$W/x"
        touch "$W/sysroot/.done"
    fi
}

R=$W/sysroot/usr/include
MIG="$repo/third_party/mig"
MIGSRC="joistmig.c error.c global.c header.c lexxer.c parser.c routine.c server.c statement.c string.c type.c user.c utils.c"

# Build joistmig from SRC (a copy of third_party/mig, perhaps mutated) and
# generate io, fs and fsys with their tests into OUT.
generate() { # SRC OUT
    rm -rf "$2" && mkdir -p "$2/inc"
    # shellcheck disable=SC2086
    (cd "$1" && clang -O2 -w -I. -o "$2/joistmig" $MIGSRC)
    cp "$3" "$2/inc/joist_mig.h"
    cp "$here/jt.h" "$here/transport.c" "$here/fuzz_demux.c" "$2/inc/"
    for d in io fs fsys; do
        clang -E -x c -nostdinc -I"$R/x86_64-gnu" -I"$R" -D__x86_64__ -DSERVERPREFIX=S_ \
            "$R/x86_64-gnu/hurd/$d.defs" -o "$2/$d.defsi"
        (cd "$2" && ./joistmig -subrprefix __ -user "${d}User.c" -header "$d.h" -server "${d}Server.c" \
            -sheader "${d}_S.h" -test "${d}Test.c" < "$d.defsi")
    done
}

cc_hurd() { # OPT SANITIZERS DIR FILE OUT
    clang --target=x86_64-unknown-linux-gnu -nostdinc -isystem "$(clang -print-resource-dir)/include" \
        -isystem "$R/x86_64-gnu" -isystem "$R" -U__linux__ -D__GNU__=1 -D__gnu_hurd__=1 -D__MACH__=1 \
        -D_GNU_SOURCE -std=gnu99 "$1" -g -Wall -Wextra -Werror -Wno-unused-parameter "$2" \
        -fno-sanitize-recover=all -I"$3/inc" -I"$3" -c "$4" -o "$5"
}

roundtrip() { # DIR OPT: 0 if every call round trips
    rm -f "$1"/*.o
    for f in "$1"/*User.c "$1"/*Server.c "$1"/*Test.c "$1/inc/transport.c"; do
        cc_hurd "$2" -fsanitize=address,undefined "$1" "$f" "$1/$(basename "$f" .c).o" || return 1
    done
    clang -fsanitize=address,undefined -o "$1/migtest" "$1"/*.o || return 1
    "$1/migtest" "$SEEDS"
}

fuzz() { # DIR SECONDS
    mkdir -p "$1/fz" "$1/corpus"
    rm -f "$1"/fz/*.o
    for f in "$1"/*User.c "$1"/*Server.c "$1"/*Test.c "$1/inc/fuzz_demux.c"; do
        cc_hurd -O1 -fsanitize=fuzzer-no-link,address,undefined "$1" "$f" "$1/fz/$(basename "$f" .c).o" || return 1
    done
    clang -fsanitize=fuzzer,address,undefined -o "$1/fuzz" "$1"/fz/*.o || return 1
    "$1/fuzz" -max_total_time="$2" -max_len=4400 -artifact_prefix="$1/" "$1/corpus" > "$1/fuzz.log" 2>&1
}

sysroot
case $mode in
test)
    generate "$MIG" "$W/gen" "$repo/user/lib/joist_mig.h"
    for o in -O0 -O1 -O2; do
        printf '%s: ' "$o"
        roundtrip "$W/gen" "$o"
    done
    ;;
fuzz)
    generate "$MIG" "$W/gen" "$repo/user/lib/joist_mig.h"
    fuzz "$W/gen" "${FUZZ_TIME:-60}" && tail -1 "$W/gen/fuzz.log" && echo "fuzz: no crash"
    ;;
mutate)
    fail=0
    grep -v '^#' "$here/mutations.txt" | grep -v '^[[:space:]]*$' | sed 's/ @@ /\x1f/g' |
    while IFS="$(printf '\037')" read -r name file from to; do
        m=$W/mut/$name
        rm -rf "$m" && mkdir -p "$m/src"
        cp -r "$MIG/." "$m/src/"
        cp "$repo/user/lib/joist_mig.h" "$m/joist_mig.h"
        target=$m/src/$file
        [ "$file" = joist_mig.h ] && target=$m/joist_mig.h
        if ! python3 -I - "$target" "$from" "$to" <<'EOF'
import sys
p, a, b = sys.argv[1:4]
s = open(p).read()
if s.count(a) != 1:
    sys.exit(1)
open(p, "w").write(s.replace(a, b))
EOF
        then
            echo "STALE    $name: text not found once"
            continue
        fi
        if ! generate "$m/src" "$m/gen" "$m/joist_mig.h" > "$m/gen.log" 2>&1; then
            echo "caught   $name: (generation fails)"
            continue
        fi
        if ! SEEDS=200 roundtrip "$m/gen" -O1 > "$m/rt.log" 2>&1; then
            echo "caught   $name: $(grep -m1 'mig-test:\|ERROR\|error' "$m/rt.log" | cut -c1-90)"
        elif ! fuzz "$m/gen" "${FUZZ_TIME:-20}"; then
            echo "caught   $name: (the fuzzer)"
        else
            echo "SURVIVED $name"
        fi
    done
    ;;
*)
    echo "usage: $0 WORKDIR [test|fuzz|mutate]" >&2
    exit 2
    ;;
esac
