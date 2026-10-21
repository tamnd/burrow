#!/bin/sh
# Regenerates tests/cmac_test_gen.h: the Wycheproof vectors Go's
# crypto/internal/fips140test CMAC test fetches as a module when it runs.
#
# Go's CMAC is in crypto/internal/fips140/aes/gcm, which a program outside the
# standard library cannot import, so the program below carries a copy of its
# cmac.go, on crypto/aes. That copy has to give what each test expects or this
# stops, so a vector Go would fail never gets in.
#
# Needs the network for the module, and the Go on PATH should be the release
# the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/cmac_test_gen.h"
goroot=$(go env GOROOT)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The version Go's tests pin, from cryptotest/wycheproof/schemaversion.go.
wycheproof_version=$(sed -n 's/^const wycheproofVersion = "\(.*\)"$/\1/p' \
	"$goroot/src/crypto/internal/cryptotest/wycheproof/schemaversion.go")
wycheproof_dir=$( (cd "$tmp" && go mod download -json "github.com/c2sp/wycheproof@$wycheproof_version") |
	sed -n 's/^[[:space:]]*"Dir": "\(.*\)",$/\1/p')

mkdir "$tmp/gen"
cat >"$tmp/gen/main.go" <<'GO'
package main

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
)

// C string literal of s, with octal escapes so nothing runs into the next
// character. A C99 compiler only has to take 4095 characters in one.
func lit(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '?':
			b.WriteString(`\077`)
		case c >= ' ' && c <= '~':
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteByte('"')
	if b.Len() > 4095+2 {
		log.Fatalf("literal of %d characters is too long", b.Len()-2)
	}
	return b.String()
}

func unhex(s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		log.Fatalf("invalid hex %q: %v", s, err)
	}
	return b
}

// Go's crypto/internal/fips140/aes/gcm/cmac.go, on a cipher.Block from
// crypto/aes in place of the internal aes.Block.
type CMAC struct {
	b  cipher.Block
	k1 [aes.BlockSize]byte
	k2 [aes.BlockSize]byte
}

func NewCMAC(b cipher.Block) *CMAC {
	c := &CMAC{b: b}
	c.deriveSubkeys()
	return c
}

func (c *CMAC) deriveSubkeys() {
	c.b.Encrypt(c.k1[:], c.k1[:])
	msb := shiftLeft(&c.k1)
	c.k1[len(c.k1)-1] ^= msb * 0b10000111

	c.k2 = c.k1
	msb = shiftLeft(&c.k2)
	c.k2[len(c.k2)-1] ^= msb * 0b10000111
}

func (c *CMAC) MAC(m []byte) [aes.BlockSize]byte {
	var x [aes.BlockSize]byte
	if len(m) == 0 {
		// Special-cased as a single empty partial final block.
		x = c.k2
		x[len(m)] ^= 0b10000000
		c.b.Encrypt(x[:], x[:])
		return x
	}
	for len(m) >= aes.BlockSize {
		subtle.XORBytes(x[:], m[:aes.BlockSize], x[:])
		if len(m) == aes.BlockSize {
			// Final complete block.
			subtle.XORBytes(x[:], c.k1[:], x[:])
		}
		c.b.Encrypt(x[:], x[:])
		m = m[aes.BlockSize:]
	}
	if len(m) > 0 {
		// Final incomplete block.
		subtle.XORBytes(x[:], m, x[:])
		subtle.XORBytes(x[:], c.k2[:], x[:])
		x[len(m)] ^= 0b10000000
		c.b.Encrypt(x[:], x[:])
	}
	return x
}

// shiftLeft sets x to x << 1, and returns MSB₁(x).
func shiftLeft(x *[aes.BlockSize]byte) byte {
	var msb byte
	for i := len(x) - 1; i >= 0; i-- {
		msb, x[i] = x[i]>>7, x[i]<<1|msb
	}
	return msb
}

func main() {
	const file = "aes_cmac_test.json"
	data, err := os.ReadFile(filepath.Join(os.Args[1], file))
	if err != nil {
		log.Fatal(err)
	}
	var wp struct {
		TestGroups []struct {
			KeySize int `json:"keySize"`
			TagSize int `json:"tagSize"`
			Tests   []struct {
				TcId    int      `json:"tcId"`
				Comment string   `json:"comment"`
				Key     string   `json:"key"`
				Msg     string   `json:"msg"`
				Tag     string   `json:"tag"`
				Result  string   `json:"result"`
				Flags   []string `json:"flags"`
			} `json:"tests"`
		} `json:"testGroups"`
	}
	if err := json.Unmarshal(data, &wp); err != nil {
		log.Fatal(err)
	}

	fmt.Println("/* The Wycheproof tests: the file, the tcId, the comment, the key, the")
	fmt.Println(" * message and the tag in hex, and whether the test is valid. Only the")
	fmt.Println(" * groups TestCMACWycheproof runs are here, the ones with a key size AES")
	fmt.Println(" * takes and 128 bit tags. */")
	fmt.Println("static const GenWycheproof gen_wycheproof[] = {")
	for _, tg := range wp.TestGroups {
		switch tg.KeySize {
		case 128, 192, 256:
		default:
			continue
		}
		if tg.TagSize != 128 {
			continue
		}
		for _, tv := range tg.Tests {
			// wycheproof.ShouldPass with no flags that pass.
			var wantPass bool
			switch tv.Result {
			case "valid":
				wantPass = true
			case "invalid":
				wantPass = false
			case "acceptable":
				wantPass = true
				for _, f := range tv.Flags {
					log.Fatalf("%s #%d: unspecified flag: %q", file, tv.TcId, f)
				}
			default:
				log.Fatalf("%s #%d: unexpected result %q", file, tv.TcId, tv.Result)
			}
			b, err := aes.NewCipher(unhex(tv.Key))
			if err != nil {
				log.Fatalf("%s #%d: aes.NewCipher: %v", file, tv.TcId, err)
			}
			tag := NewCMAC(b).MAC(unhex(tv.Msg))
			if bytes.Equal(tag[:], unhex(tv.Tag)) != wantPass {
				log.Fatalf("%s #%d: the tag does not give the expected result", file, tv.TcId)
			}
			fmt.Printf("    {%s, %d, %s, %s, %s, %s, %v},\n", lit(file), tv.TcId,
				lit(tv.Comment), lit(tv.Key), lit(tv.Msg), lit(tv.Tag), wantPass)
		}
	}
	fmt.Println("};")
}
GO

{
	cat <<EOF
/* Generated by tools/gen-cmac-tests.sh; do not edit.
 *
 * The Wycheproof vectors Go's CMAC test reads, from $(go env GOVERSION) and
 * github.com/c2sp/wycheproof at $wycheproof_version. */

#ifndef BURROW_TESTS_CMAC_TEST_GEN_H
#define BURROW_TESTS_CMAC_TEST_GEN_H

#include <stdbool.h>

typedef struct {
    const char *file;
    int tc_id;
    const char *comment;
    const char *key, *msg, *tag;
    bool want_pass;
} GenWycheproof;

EOF
	(cd "$tmp/gen" && GO111MODULE=off go run main.go "$wycheproof_dir/testvectors_v1")
	cat <<'EOF'

#endif /* BURROW_TESTS_CMAC_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
