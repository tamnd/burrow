#!/bin/sh
# The size table in docs/design/15-build-deploy.md, measured. Each program is
# built from the amalgamation the way a program that uses it would be, with
# every function and object in its own section so the linker can drop what
# nothing reaches, and stripped. It is built twice, once on the full
# amalgamation and once on one made with --packages and the packages it uses.
# Its Go counterpart in tools/size, when there is one and a go command, is
# built with the symbol table and DWARF left out, which is as close to
# stripped as go build gets.
#
#     tools/size.sh
#
# CC picks the compiler and GO the go command, which can be "" to leave Go
# out. It prints one line per program: its name, its size in bytes on the full
# amalgamation, its size on the --packages one, and the Go binary's size, or
# "-" when there isn't one. The last line is the whole library, which is the
# text, data and bss of burrow.o as size(1) on Linux prints it.
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

cflags="-std=c11 -O2 -ffunction-sections -fdata-sections"
case $(uname) in
    Darwin) gc=-Wl,-dead_strip ;;
    *) gc=-Wl,--gc-sections ;;
esac

# amalgamate DIR [PACKAGES] writes the pair to DIR and compiles burrow.o
# there. The programs include burrow/burrow.h, so DIR/include gets one that
# includes the amalgamated burrow.h.
amalgamate() {
    if [ $# -gt 1 ]; then
        "$root/tools/burrow-gen" amalgamate --packages "$2" --out "$1" > /dev/null
    else
        "$root/tools/burrow-gen" amalgamate --out "$1" > /dev/null
    fi
    mkdir -p "$1/include/burrow"
    echo '#include "../../burrow.h"' > "$1/include/burrow/burrow.h"
    # shellcheck disable=SC2086
    "$cc" $cflags -c "$1/burrow.c" -o "$1/burrow.o"
}

# link DIR SRC OUT links a program against DIR's burrow.o and prints its size.
link() {
    # shellcheck disable=SC2086
    "$cc" $cflags -I"$1/include" "$2" "$1/burrow.o" $gc -s -pthread -lm -o "$3"
    wc -c < "$3" | tr -d ' '
}

amalgamate "$tmp/full"

# The programs in tools/size come first, then cookbook recipes for what they
# don't cover. Each has the packages it uses, and --packages brings in what
# those need.
while read -r name packages; do
    src=$root/tools/size/$name.c
    [ -f "$src" ] || src=$root/docs/examples/cookbook/$name.c
    full=$(link "$tmp/full" "$src" "$tmp/$name" < /dev/null)
    amalgamate "$tmp/$name.p" "$packages" < /dev/null
    picked=$(link "$tmp/$name.p" "$src" "$tmp/$name.p/$name" < /dev/null)
    rm -rf "$tmp/$name.p"
    gosize=-
    if [ -n "$go" ] && [ -f "$root/tools/size/$name.go" ]; then
        (cd "$root/tools/size" && $go build -trimpath -ldflags="-s -w" -o "$tmp/$name-go" "$name.go" < /dev/null)
        gosize=$(wc -c < "$tmp/$name-go" | tr -d ' ')
    fi
    echo "$name $full $picked $gosize"
done << EOF
floor strings,strconv,time
hello fmt
slog_logs log/slog,os
json_config encoding/json,fmt,os
worker_pool fmt,sync
http net/http,fmt
https net/http,fmt
EOF
echo "library $(size "$tmp/full/burrow.o" | awk 'NR == 2 { print $4 }') - -"
