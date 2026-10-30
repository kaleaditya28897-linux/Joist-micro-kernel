#!/bin/sh
# check-toolchain.sh - is this the verified toolchain? (decision 0026,
# layer 3; docs/toolchain/record.md)
#
# Checks every file of a toolchain directory against the hashes recorded
# for the verified LLVM 21.1.8 (docs/toolchain/llvm-21.1.8-verified.SHA256SUMS),
# and that it holds no other file. Symlinks are checked to point where the
# record says. Exits 0 only if all of it matches.
#
# usage: tools/ddc/check-toolchain.sh DIR   (e.g. the directory above LLVM_BIN)
set -u
top=$(cd "$(dirname "$0")/../.." && pwd)
sums=$top/docs/toolchain/llvm-21.1.8-verified.SHA256SUMS
dir=$1
cd "$dir" || exit 1
LC_ALL=C
export LC_ALL
fail=0

if ! sha256sum --quiet -c "$sums"; then
    echo "check-toolchain: files differ from the record"
    fail=1
fi
# No file the record doesn't list (SHA256SUMS itself aside).
extra=$(find . -type f ! -path ./SHA256SUMS | sort |
    while read -r f; do grep -q -F "  $f" "$sums" || echo "$f"; done)
if [ -n "$extra" ]; then
    echo "check-toolchain: files the record doesn't list:"
    echo "$extra"
    fail=1
fi
# The symlinks, as the build installs them.
for l in clang:clang-21 clang++:clang clang-cpp:clang clang-cl:clang \
    ld.lld:lld ld64.lld:lld lld-link:lld wasm-ld:lld llvm-readelf:llvm-readobj; do
    name=${l%%:*} target=${l#*:}
    [ "$(readlink "bin/$name")" = "$target" ] ||
        { echo "check-toolchain: bin/$name does not point to $target"; fail=1; }
done
[ $fail -eq 0 ] && echo "check-toolchain: $dir is the verified LLVM 21.1.8 ($(wc -l < "$sums") files)"
exit $fail
