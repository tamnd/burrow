#!/bin/sh
# Regenerate the tables in src/strconv/isprint.c from Go's strconv/isprint.go.
#
# Go generates its isprint.go from the unicode tables with makeisprint.go, and
# this turns that output into C rather than repeating the generation, so the
# tables are Go's entry for entry. It rewrites the part of isprint.c between
# the BEGIN and END GENERATED TABLES markers and leaves the code around them
# alone. Run it when moving to a new Go release, and update the counts and
# hashes in tests/strconv_quote_test.c with it. It needs Go only to find the
# file, and takes the path instead if given one.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

src="${1:-$(go env GOROOT)/src/strconv/isprint.go}"
out=src/strconv/isprint.c
tmp=$(mktemp)
trap 'rm -f "$tmp" "$tmp.c"' EXIT

{
	printf '/* clang-format off */\n'
	sed -n '/^package strconv/,$p' "$src" | sed '1d' |
		sed -e 's|^var \([A-Za-z0-9]*\) = \[\]\(uint[0-9]*\){\(.*\)$|static const \2_t \1[] = {\3|' \
			-e 's|^}$|};|' \
			-e 's|// \(.*\)$|/* \1 */|' \
			-e 's|\t|    |g' \
			-e 's|isPrint|is_print|; s|isNotPrint|is_not_print|; s|isGraphic|is_graphic|' | cat -s
	printf '\n/* clang-format on */\n'
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
