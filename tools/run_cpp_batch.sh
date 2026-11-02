#!/bin/sh
# Optional Clang/macOS test recipe. All work remains inside the caller's sandbox.
set -eu
if [ "$#" -lt 5 ] || [ "$#" -gt 36 ]; then
    echo 'usage: run_cpp_batch.sh plain|pch TARGET HEADER OUTPUT SOURCE...' >&2
    exit 2
fi
mode=$1; target=$2; header=$3; output=$4; shift 4
case "$mode" in plain|pch) ;; *) exit 2;; esac
if ! printf '%s\n' "$target" | /usr/bin/grep -Eq '^[0-9]{1,3}(\.[0-9]{1,3}){1,2}$'; then exit 2; fi
[ -f "$header" ] || exit 2
for source do [ -f "$source" ] || exit 2; done
compiler=${CXX:-c++}
work=$(mktemp -d "${TMPDIR:-/tmp}/cxf-cpp-XXXXXX") || exit 2
cleanup() {
    rc=$?
    trap - 0 HUP INT TERM
    if ! rm -rf "$work"; then rc=1; fi
    exit "$rc"
}
trap cleanup 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
if [ "$mode" = pch ]; then
    "$compiler" -std=c++17 -O2 "-mmacosx-version-min=$target" -x c++-header "$header" -o "$work/common.pch"
fi
index=0
for source do
    index=$((index+1))
    object=$(printf '%s/unit-%06d.o' "$work" "$index")
    if [ "$mode" = pch ]; then
        "$compiler" -std=c++17 -O2 "-mmacosx-version-min=$target" -include-pch "$work/common.pch" -c "$source" -o "$object"
    else
        "$compiler" -std=c++17 -O2 "-mmacosx-version-min=$target" -include "$header" -c "$source" -o "$object"
    fi
done
set -- "$work"/unit-*.o
"$compiler" "-mmacosx-version-min=$target" "$@" -o "$output"
"$output"
