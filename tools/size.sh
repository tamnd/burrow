#!/bin/sh
# The size table in docs/design/15-build-deploy.md, measured. Each program is
# built from the amalgamation the way a program that uses it would be, with
# every function and object in its own section so the linker can drop what
# nothing reaches, and stripped. Its Go counterpart in tools/size, when there
# is one and a go command, is built with the symbol table and DWARF left out,
# which is as close to stripped as go build gets.
#
#     tools/size.sh
#
# CC picks the compiler and GO the go command, which can be "" to leave Go
# out. It prints one line per program: its name, its size in bytes and the Go
# binary's size in bytes, or "-" when there isn't one. The last line is the
# whole library, which is the text, data and bss of burrow.o as size(1) on
# Linux prints it.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
go=${GO-go}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$root/tools/burrow-gen" amalgamate --out "$tmp" > /dev/null
cflags="-std=c11 -O2 -ffunction-sections -fdata-sections"
case $(uname) in
    Darwin) gc=-Wl,-dead_strip ;;
    *) gc=-Wl,--gc-sections ;;
esac
# shellcheck disable=SC2086
"$cc" $cflags -c "$tmp/burrow.c" -o "$tmp/burrow.o"

# The programs in tools/size come first, then cookbook recipes for what they
# don't cover.
for name in floor hello slog_logs json_config worker_pool http https; do
    src=$root/tools/size/$name.c
    [ -f "$src" ] || src=$root/docs/examples/cookbook/$name.c
    # shellcheck disable=SC2086
    "$cc" $cflags -I"$root/include" "$src" "$tmp/burrow.o" $gc -s -pthread -lm -o "$tmp/$name"
    size=$(wc -c < "$tmp/$name" | tr -d ' ')
    gosize=-
    if [ -n "$go" ] && [ -f "$root/tools/size/$name.go" ]; then
        (cd "$root/tools/size" && $go build -trimpath -ldflags="-s -w" -o "$tmp/$name-go" "$name.go")
        gosize=$(wc -c < "$tmp/$name-go" | tr -d ' ')
    fi
    echo "$name $size $gosize"
done
echo "library $(size "$tmp/burrow.o" | awk 'NR == 2 { print $4 }') -"
