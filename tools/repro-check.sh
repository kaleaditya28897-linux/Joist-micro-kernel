#!/bin/sh
# repro-check.sh - are Joist's builds reproducible? (decision 0026, layer 1)
#
# Copies the tree to two directories with different paths and builds the
# kernel, every user program and an ISO (the memory manager's test, with its
# modules) in each: once with one time zone, locale and umask, once with
# others, at different times. Then compares every artifact byte for byte.
# Any difference fails the check, and diffoscope (if installed) shows it.
#
# usage: tools/repro-check.sh   (from the top of the tree; uses $TMPDIR)
set -u
top=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/joist-repro.XXXXXX")
trap 'rm -rf "$work"' EXIT
root=mmroot
mods="mm mmclient"

build() { # DIR TZ LOCALE UMASK
    mkdir -p "$1"
    rsync -a --exclude .git --exclude build --exclude __pycache__ --exclude '*.o' \
        --exclude .claude --exclude 'mcp-server/joist-mcp' "$top"/ "$1"/
    (
        cd "$1" || exit 1
        umask "$4"
        export TZ="$2" LC_ALL="$3"
        make -s -C user >/dev/null &&
            make -s -C kernel ROOT_TASK=$root MODULES="$mods" build/joist.iso >/dev/null 2>&1
    ) || { echo "repro-check: the build in $1 failed"; exit 1; }
}

build "$work/a/joist" UTC C 022
sleep 2 # a different second: any date taken from the clock shows
build "$work/b/some/longer/path/joist-tree" Asia/Kolkata C.UTF-8 077

fail=0
n=0
for f in kernel/build/joist.elf kernel/build/joist.iso $(cd "$work/a/joist" && ls user/build/*.elf); do
    n=$((n + 1))
    if ! cmp -s "$work/a/joist/$f" "$work/b/some/longer/path/joist-tree/$f"; then
        echo "repro-check: $f differs"
        fail=1
        if command -v diffoscope >/dev/null; then
            diffoscope "$work/a/joist/$f" "$work/b/some/longer/path/joist-tree/$f" | head -40
        fi
    fi
done
[ $fail -eq 0 ] && echo "repro-check: all $n artifacts identical across two builds"
exit $fail
