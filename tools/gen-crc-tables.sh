#!/bin/sh
# Regenerate the constant tables in src/hash/crc32.c and src/hash/crc64.c.
#
# Go builds the CRC tables when the package starts. burrow keeps the ones for
# the named polynomials as constants instead, so nothing has to run first and a
# table can never be read half built. This prints them from Go's own
# crc32.MakeTable and crc64.MakeTable, so the entries are Go's, and rewrites the
# part of each file between the BEGIN and END GENERATED TABLES markers. The Go
# on PATH should be the release the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat >"$work/main.go" <<'GO'
package main

import (
	"fmt"
	"hash/crc32"
	"hash/crc64"
	"os"
)

func main() {
	if os.Args[1] == "32" {
		for _, p := range []struct {
			name string
			poly uint32
		}{{"ieee", crc32.IEEE}, {"castagnoli", crc32.Castagnoli}} {
			t := crc32.MakeTable(p.poly)
			fmt.Printf("static const Crc32Table %s_table = {{\n", p.name)
			for i := 0; i < 256; i += 6 {
				fmt.Print("   ")
				for j := i; j < i+6 && j < 256; j++ {
					fmt.Printf(" 0x%08x,", t[j])
				}
				fmt.Println()
			}
			fmt.Println("}};")
		}
		return
	}
	for _, p := range []struct {
		name string
		poly uint64
	}{{"iso", crc64.ISO}, {"ecma", crc64.ECMA}} {
		t := crc64.MakeTable(p.poly)
		fmt.Printf("static const Crc64Table %s_table = {{\n", p.name)
		for i := 0; i < 256; i += 3 {
			fmt.Print("   ")
			for j := i; j < i+3 && j < 256; j++ {
				fmt.Printf(" 0x%016xULL,", t[j])
			}
			fmt.Println()
		}
		fmt.Println("}};")
	}
}
GO

for bits in 32 64; do
	out=src/hash/crc$bits.c
	{
		printf '/* clang-format off */\n'
		(cd "$work" && go run main.go $bits)
		printf '/* clang-format on */\n'
	} >"$work/tables"
	awk -v tables="$work/tables" '
		/^ *\/\* END GENERATED TABLES \*\/$/ { skip = 0 }
		!skip { print }
		/^ *\/\* BEGIN GENERATED TABLES \*\/$/ {
			while ((getline line < tables) > 0) print line
			skip = 1
		}
	' "$out" >"$work/out.c"
	mv "$work/out.c" "$out"
done
