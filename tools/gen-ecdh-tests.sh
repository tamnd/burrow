#!/bin/sh
# Regenerates tests/ecdh_test_gen.h: the Wycheproof vectors Go's crypto/ecdh
# tests fetch as a module when they run. Each one goes through what Go's
# runECDHWycheproofTest does with it, with Go's own crypto/ecdh and
# crypto/x509, and this stops if Go would fail it, so a vector Go would fail
# never gets in.
#
# Needs the network for the module, and the Go on PATH should be the release
# the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/ecdh_test_gen.h"
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
	"crypto/ecdh"
	"crypto/ecdsa"
	"crypto/x509"
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

// hex as up to three C string literals in braces, since the longest public
// keys are longer than one literal can be.
func parts(s string) string {
	n := len(s)
	var p []string
	for len(s) > 4000 {
		p = append(p, lit(s[:4000]))
		s = s[4000:]
	}
	p = append(p, lit(s))
	if len(p) > 3 {
		log.Fatalf("hex of %d characters does not fit", n)
	}
	return "{" + strings.Join(p, ", ") + "}"
}

func unhex(s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		log.Fatalf("invalid hex %q: %v", s, err)
	}
	return b
}

// The curves, by the number the C table gives them, with the key size
// runECDHWycheproofTest expects.
var curves = map[string]struct {
	index   int
	curve   ecdh.Curve
	keySize int
}{
	"secp256r1":  {0, ecdh.P256(), 32},
	"secp384r1":  {1, ecdh.P384(), 48},
	"secp521r1":  {2, ecdh.P521(), 66},
	"curve25519": {3, ecdh.X25519(), 32},
}

func parseSPKIPub(p []byte) (*ecdh.PublicKey, error) {
	pubKeyAny, err := x509.ParsePKIXPublicKey(p)
	if err != nil {
		return nil, err
	}
	ecdsaPub, ok := pubKeyAny.(*ecdsa.PublicKey)
	if !ok {
		return nil, fmt.Errorf("unexpected key type %T", pubKeyAny)
	}
	return ecdsaPub.ECDH()
}

// The three tests: the table each goes in, the files it reads, the curves it
// takes from them, its flagsShouldPass, and how it parses the public key.
var tests = []struct {
	table           string
	files           []string
	curves          []string
	flagsShouldPass map[string]bool
	spki            bool
}{
	{
		"gen_ecpoint",
		[]string{
			"ecdh_secp256r1_ecpoint_test.json",
			"ecdh_secp384r1_ecpoint_test.json",
			"ecdh_secp521r1_ecpoint_test.json",
		},
		[]string{"secp256r1", "secp384r1", "secp521r1"},
		map[string]bool{
			"CompressedPoint":  false,
			"CompressedPublic": false,
		},
		false,
	},
	{
		"gen_spki",
		[]string{
			"ecdh_secp256r1_test.json",
			"ecdh_secp384r1_test.json",
			"ecdh_secp521r1_test.json",
		},
		[]string{"secp256r1", "secp384r1", "secp521r1"},
		map[string]bool{
			"CompressedPublic":         false,
			"CompressedPoint":          false,
			"UnnamedCurve":             false,
			"WrongOrder":               false,
			"UnusedParam":              false,
			"ModifiedGenerator":        false,
			"ModifiedCofactor":         false,
			"ModifiedCurveParameter":   false,
			"NoCofactor":               false,
			"Modified curve parameter": false,
			"InvalidAsn":               false,
		},
		true,
	},
	{
		"gen_x25519",
		[]string{"x25519_test.json"},
		[]string{"curve25519"},
		map[string]bool{
			"Twist":                  true,
			"SmallPublicKey":         false,
			"LowOrderPublic":         false,
			"ZeroSharedSecret":       false,
			"NonCanonicalPublic":     true,
			"SpecialPublicKey":       true,
			"EdgeCaseMultiplication": true,
			"EdgeCaseShared":         true,
			"Ktv":                    true,
		},
		false,
	},
}

func main() {
	wycheproofDir := os.Args[1]

	for _, test := range tests {
		var out strings.Builder
		for _, file := range test.files {
			data, err := os.ReadFile(filepath.Join(wycheproofDir, file))
			if err != nil {
				log.Fatal(err)
			}
			var wp struct {
				TestGroups []struct {
					Curve string `json:"curve"`
					Tests []struct {
						TcId    int      `json:"tcId"`
						Comment string   `json:"comment"`
						Public  string   `json:"public"`
						Private string   `json:"private"`
						Shared  string   `json:"shared"`
						Result  string   `json:"result"`
						Flags   []string `json:"flags"`
					} `json:"tests"`
				} `json:"testGroups"`
			}
			if err := json.Unmarshal(data, &wp); err != nil {
				log.Fatal(err)
			}
			for _, tg := range wp.TestGroups {
				ok := false
				for _, c := range test.curves {
					ok = ok || c == tg.Curve
				}
				if !ok {
					continue
				}
				c := curves[tg.Curve]
				for _, tv := range tg.Tests {
					// wycheproof.ShouldPass.
					var shouldPass bool
					switch tv.Result {
					case "valid":
						shouldPass = true
					case "invalid":
						shouldPass = false
					case "acceptable":
						shouldPass = true
						for _, flag := range tv.Flags {
							pass, ok := test.flagsShouldPass[flag]
							if !ok {
								log.Fatalf("%s #%d: unspecified flag: %q", file, tv.TcId, flag)
							}
							if !pass {
								shouldPass = false
								break
							}
						}
					default:
						log.Fatalf("%s #%d: unexpected result %q", file, tv.TcId, tv.Result)
					}
					invalid := tv.Result == "invalid"
					fail := func(format string, args ...any) {
						log.Fatalf("%s #%d: Go: %s", file, tv.TcId, fmt.Sprintf(format, args...))
					}

					// runECDHWycheproofTest.
					func() {
						var pub *ecdh.PublicKey
						if test.spki {
							pub, err = parseSPKIPub(unhex(tv.Public))
						} else {
							pub, err = c.curve.NewPublicKey(unhex(tv.Public))
						}
						if err != nil {
							if shouldPass {
								fail("parsePub: %v", err)
							}
							return
						}
						privBytes := unhex(tv.Private)
						priv, err := c.curve.NewPrivateKey(privBytes)
						if err != nil {
							if shouldPass && len(privBytes) == c.keySize {
								fail("NewPrivateKey: %v", err)
							}
							return
						}
						x, err := priv.ECDH(pub)
						if err != nil {
							if shouldPass {
								fail("ECDH: %v", err)
							}
							return
						}
						shared := unhex(tv.Shared)
						if shouldPass {
							if !bytes.Equal(shared, x) {
								fail("ECDH = %x, want %x", x, shared)
							}
						} else if invalid {
							if bytes.Equal(shared, x) {
								fail("ECDH = %x, want anything else", x)
							}
						}
					}()

					fmt.Fprintf(&out, "    {%s, %d, %s, %d, %s, %s, %s, %v, %v},\n", lit(file),
						tv.TcId, lit(tv.Comment), c.index, parts(tv.Public), lit(tv.Private),
						lit(tv.Shared), shouldPass, invalid)
				}
			}
		}
		fmt.Printf("static const GenWycheproof %s[] = {\n", test.table)
		fmt.Print(out.String())
		fmt.Printf("};\n")
		if test.table != tests[len(tests)-1].table {
			fmt.Println()
		}
	}
}
GO

{
	cat <<EOF
/* Generated by tools/gen-ecdh-tests.sh; do not edit.
 *
 * The Wycheproof vectors Go's crypto/ecdh tests read, from $(go env GOVERSION)
 * and github.com/c2sp/wycheproof at $wycheproof_version. */

#ifndef BURROW_TESTS_ECDH_TEST_GEN_H
#define BURROW_TESTS_ECDH_TEST_GEN_H

#include <stdbool.h>

/* The Wycheproof tests: the file, the tcId, the comment, the curve (0 for
 * P-256, 1 for P-384, 2 for P-521 and 3 for X25519), the public key in hex in
 * up to three pieces, the private key and the shared secret in hex, what
 * ShouldPass says with the test's flagsShouldPass, and whether the result is
 * invalid. gen_ecpoint is TestSecpECDHWycheproof's, gen_spki is
 * TestSecpECDHSPKIWycheproof's and gen_x25519 is TestX25519ECDHWycheproof's. */
typedef struct {
    const char *file;
    int tc_id;
    const char *comment;
    int curve;
    const char *public_key[3];
    const char *private_key, *shared;
    bool should_pass, invalid;
} GenWycheproof;

EOF
	(cd "$tmp/gen" && GO111MODULE=off go run main.go "$wycheproof_dir/testvectors_v1")
	cat <<'EOF'

#endif /* BURROW_TESTS_ECDH_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
