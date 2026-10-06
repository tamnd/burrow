#!/bin/sh
# Regenerate src/crypto/edwards25519_table.c.
#
# Go builds two tables for the edwards25519 generator the first time a scalar
# multiplication needs them: 256^i times the generator for i up to 31, and its
# odd multiples up to 127. burrow keeps them as constants, so nothing has to
# run first and a table can never be read half built. The tests build both
# again with the C arithmetic and compare, so the constants have to come from
# somewhere else, and this prints them from Go's own code.
#
# Go's edwards25519 is internal to the standard library and cannot be
# imported, so this copies it and its field package out of GOROOT into a
# GOPATH of its own, with small packages standing in for the internal ones it
# imports, and adds one file to each that hands the tables and the limbs out.
# The Go on PATH should be the release the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

src=$(go env GOROOT)/src/crypto/internal/fips140/edwards25519
version=$(go env GOVERSION)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

pkg=$work/src/edw
mkdir -p "$pkg/edwards25519/field" "$pkg/byteorder" "$pkg/constanttime" "$work/src/gen"

# Every non-test Go file but the amd64 assembly's, with the imports pointed at
# the copies and the stand-ins.
copy_go() {
	for f in "$1"/*.go; do
		case $f in
		*_test.go | */fe_amd64.go) continue ;;
		esac
		sed -e '/"crypto\/internal\/fips140\/check"/d' \
			-e 's#"crypto/internal/fips140/edwards25519/field"#"edw/edwards25519/field"#' \
			-e 's#"crypto/internal/fips140/subtle"#"crypto/subtle"#' \
			-e 's#"crypto/internal/fips140deps/byteorder"#"edw/byteorder"#' \
			-e 's#"crypto/internal/constanttime"#"edw/constanttime"#' \
			"$f" >"$2/$(basename "$f")"
	done
}
copy_go "$src" "$pkg/edwards25519"
copy_go "$src/field" "$pkg/edwards25519/field"

cat >"$pkg/byteorder/byteorder.go" <<'GO'
package byteorder

import "encoding/binary"

func LEUint64(b []byte) uint64       { return binary.LittleEndian.Uint64(b) }
func LEPutUint64(b []byte, v uint64) { binary.LittleEndian.PutUint64(b, v) }
GO

cat >"$pkg/constanttime/constanttime.go" <<'GO'
package constanttime

import "crypto/subtle"

func ByteEq(x, y uint8) int { return subtle.ConstantTimeByteEq(x, y) }
GO

cat >"$pkg/edwards25519/field/limbs.go" <<'GO'
package field

func (v *Element) Limbs() [5]uint64 { return [5]uint64{v.l0, v.l1, v.l2, v.l3, v.l4} }
GO

cat >"$pkg/edwards25519/dump.go" <<'GO'
package edwards25519

// BasepointTables gives each entry of both tables as the limbs of YplusX,
// YminusX and T2d.
func BasepointTables() (table [32][8][3][5]uint64, naf [64][3][5]uint64) {
	limbs := func(c *affineCached) [3][5]uint64 {
		return [3][5]uint64{c.YplusX.Limbs(), c.YminusX.Limbs(), c.T2d.Limbs()}
	}
	for i, t := range basepointTable() {
		for j := range t.points {
			table[i][j] = limbs(&t.points[j])
		}
	}
	for j := range basepointNafTable().points {
		naf[j] = limbs(&basepointNafTable().points[j])
	}
	return
}
GO

cat >"$work/src/gen/main.go" <<'GO'
package main

import (
	"fmt"

	"edw/edwards25519"
)

func cached(indent string, c [3][5]uint64) {
	fmt.Printf("%s{\n", indent)
	for _, e := range c {
		fmt.Printf("%s    {0x%013x, 0x%013x, 0x%013x, 0x%013x, 0x%013x},\n",
			indent, e[0], e[1], e[2], e[3], e[4])
	}
	fmt.Printf("%s},\n", indent)
}

func main() {
	table, naf := edwards25519.BasepointTables()
	fmt.Println("const Edwards25519AffineLookupTable burrow__ge_basepoint_table[32] = {")
	for i := range table {
		fmt.Printf("    /* 256^%d * B */\n", i)
		fmt.Println("    {{")
		for j := range table[i] {
			cached("        ", table[i][j])
		}
		fmt.Println("    }},")
	}
	fmt.Println("};")
	fmt.Println()
	fmt.Println("const Edwards25519NafLookupTable8 burrow__ge_basepoint_naf_table = {{")
	for j := range naf {
		fmt.Printf("    /* %d * B */\n", 2*j+1)
		cached("    ", naf[j])
	}
	fmt.Println("}};")
}
GO

out=src/crypto/edwards25519_table.c
{
	cat <<EOF
/* Derived from Go's src/crypto/internal/fips140/edwards25519/scalarmult.go.
 * Go source: go1.27.1.
 *
 * The two tables for the edwards25519 generator B that Go builds the first time
 * they are used, as tools/gen-ed25519-tables.sh printed them from Go's own code
 * at $version. Each entry is the YplusX, YminusX and T2d of an
 * Edwards25519AffineCached, in five 51 bit limbs each. Do not edit by hand.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "edwards25519.h"

EOF
	(cd "$work/src/gen" && GOPATH=$work GO111MODULE=off go run -tags purego .)
} >"$work/out.c"
mv "$work/out.c" "$out"
