#!/bin/sh
# Regenerate the table in src/strconv/pow10tab.c from Go's
# internal/strconv/pow10tab.go.
#
# The table holds the 128 bit mantissas of 10^-348 to 10^347 that float parsing
# and formatting scale by. Go generates it with pow10gen.go, and this turns that
# output into C rather than repeating the arithmetic, so the entries are Go's.
# It rewrites the part of pow10tab.c between the BEGIN and END GENERATED TABLES
# markers and leaves the rest alone. It needs Go only to find the file, and
# takes the path instead if given one.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

src="${1:-$(go env GOROOT)/src/internal/strconv/pow10tab.go}"
out=src/strconv/pow10tab.c
tmp=$(mktemp)
trap 'rm -f "$tmp" "$tmp.c"' EXIT

{
	printf '/* clang-format off */\n'
	grep '^	{0x' "$src" |
		sed -e 's|^	{\(0x[0-9a-f]*\), \(0x[0-9a-f]*\)}, // \(.*\)$|    {\1ULL, \2ULL}, /* \3 */|'
	printf '/* clang-format on */\n'
} >"$tmp"

awk -v tables="$tmp" '
	/^\/\* END GENERATED TABLES \*\/$/ { skip = 0 }
	!skip { print }
	/^\/\* BEGIN GENERATED TABLES \*\/$/ {
		while ((getline line < tables) > 0) print line
		skip = 1
	}
' "$out" >"$tmp.c"
mv "$tmp.c" "$out"
