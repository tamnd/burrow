#!/bin/sh
# Regenerates tests/ed25519_test_gen.h: the files Go's crypto/ed25519 tests
# read. That is testdata/sign.input.gz from GOROOT, kept as it is, and two sets
# of vectors Go fetches as modules when the test runs: Filippo Valsorda's
# ed25519vectors, every mix of low order points and non-canonical encodings,
# and Wycheproof's ed25519_test.json, both at the versions Go pins. A test here
# cannot fetch anything, so they go in the header, already decoded, with what
# Go's test expects of each. Go's own Verify has to agree with that or this
# stops, so a vector Go would fail never gets in.
#
# Needs the network for the two modules, and the Go on PATH should be the
# release the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/ed25519_test_gen.h"
goroot=$(go env GOROOT)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The versions Go's tests pin, from ed25519vectors_test.go and
# cryptotest/wycheproof/schemaversion.go.
vectors_version=v0.0.0-20210322192420-30a2d7243a94
wycheproof_version=$(sed -n 's/^const wycheproofVersion = "\(.*\)"$/\1/p' \
	"$goroot/src/crypto/internal/cryptotest/wycheproof/schemaversion.go")

moddir() {
	(cd "$tmp" && go mod download -json "$1@$2") |
		sed -n 's/^[[:space:]]*"Dir": "\(.*\)",$/\1/p'
}
vectors_dir=$(moddir filippo.io/mostly-harmless/ed25519vectors "$vectors_version")
wycheproof_dir=$(moddir github.com/c2sp/wycheproof "$wycheproof_version")

mkdir "$tmp/gen"
cat >"$tmp/gen/main.go" <<'GO'
package main

import (
	"crypto/ed25519"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"strings"
)

// C string literal of s, with octal escapes so nothing runs into the next
// character.
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
	return b.String()
}

func unhex(s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		log.Fatalf("invalid hex %q: %v", s, err)
	}
	return b
}

func main() {
	signInput, vectorsFile, wycheproofFile := os.Args[1], os.Args[2], os.Args[3]

	z, err := os.ReadFile(signInput)
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println("/* testdata/sign.input.gz. */")
	fmt.Println("static const unsigned char gen_sign_input_gz[] = {")
	for i := 0; i < len(z); i += 12 {
		fmt.Print("   ")
		for _, c := range z[i:min(i+12, len(z))] {
			fmt.Printf(" 0x%02x,", c)
		}
		fmt.Println()
	}
	fmt.Println("};")
	fmt.Println()

	// TestEd25519Vectors.
	data, err := os.ReadFile(vectorsFile)
	if err != nil {
		log.Fatal(err)
	}
	var vectors []struct {
		A, R, S, M string
		Flags      []string
	}
	if err := json.Unmarshal(data, &vectors); err != nil {
		log.Fatal(err)
	}
	fmt.Println("/* ed25519vectors.json: A, R and S in hex, M, the flags, and whether Go's test")
	fmt.Println(" * expects the signature to verify. */")
	fmt.Println("static const GenVector gen_vectors[] = {")
	for i, v := range vectors {
		want := true
		for _, f := range v.Flags {
			switch f {
			case "LowOrderResidue", "NonCanonicalR":
				want = false
			}
		}
		sig := append(unhex(v.R), unhex(v.S)...)
		if got := ed25519.Verify(unhex(v.A), []byte(v.M), sig); got != want {
			log.Fatalf("vector #%d: Go's Verify = %v, want %v", i, got, want)
		}
		fmt.Printf("    {%s, %s, %s, %s, %s, %v},\n", lit(v.A), lit(v.R), lit(v.S), lit(v.M),
			lit(strings.Join(v.Flags, " ")), want)
	}
	fmt.Println("};")
	fmt.Println()

	// TestEd25519Wycheproof. Go takes the key from the DER and so does this.
	data, err = os.ReadFile(wycheproofFile)
	if err != nil {
		log.Fatal(err)
	}
	var wp struct {
		TestGroups []struct {
			PublicKeyDer string `json:"publicKeyDer"`
			Tests        []struct {
				TcId    int      `json:"tcId"`
				Comment string   `json:"comment"`
				Msg     string   `json:"msg"`
				Sig     string   `json:"sig"`
				Result  string   `json:"result"`
				Flags   []string `json:"flags"`
			} `json:"tests"`
		} `json:"testGroups"`
	}
	if err := json.Unmarshal(data, &wp); err != nil {
		log.Fatal(err)
	}
	fmt.Println("/* ed25519_test.json: the tcId, the comment, the public key, the message and")
	fmt.Println(" * the signature in hex, and whether the signature is valid. */")
	fmt.Println("static const GenWycheproof gen_wycheproof[] = {")
	for g, tg := range wp.TestGroups {
		k, err := x509.ParsePKIXPublicKey(unhex(tg.PublicKeyDer))
		if err != nil {
			log.Fatalf("test group %d invalid DER encoded public key: %v", g+1, err)
		}
		pub, ok := k.(ed25519.PublicKey)
		if !ok {
			log.Fatalf("test group %d: unexpected key type %T", g+1, k)
		}
		for _, tv := range tg.Tests {
			var want bool
			switch tv.Result {
			case "valid":
				want = true
			case "invalid":
				want = false
			default:
				// Go's test passes no flags for "acceptable", so one would stop it.
				log.Fatalf("#%d: unexpected result %q", tv.TcId, tv.Result)
			}
			if got := ed25519.Verify(pub, unhex(tv.Msg), unhex(tv.Sig)); got != want {
				log.Fatalf("#%d: Go's Verify = %v, want %v", tv.TcId, got, want)
			}
			fmt.Printf("    {%d, %s, %s, %s, %s, %v},\n", tv.TcId, lit(tv.Comment),
				lit(hex.EncodeToString(pub)), lit(tv.Msg), lit(tv.Sig), want)
		}
	}
	fmt.Println("};")
}
GO

{
	cat <<EOF
/* Generated by tools/gen-ed25519-tests.sh; do not edit.
 *
 * What Go's crypto/ed25519 tests read, from $(go env GOVERSION): sign.input.gz,
 * filippo.io/mostly-harmless/ed25519vectors at $vectors_version
 * and github.com/c2sp/wycheproof at $wycheproof_version. */

#ifndef BURROW_TESTS_ED25519_TEST_GEN_H
#define BURROW_TESTS_ED25519_TEST_GEN_H

#include <stdbool.h>

typedef struct {
    const char *a, *r, *s, *m, *flags;
    bool want;
} GenVector;

typedef struct {
    int tc_id;
    const char *comment, *pub, *msg, *sig;
    bool want;
} GenWycheproof;

EOF
	(cd "$tmp/gen" && GO111MODULE=off go run main.go \
		"$goroot/src/crypto/ed25519/testdata/sign.input.gz" \
		"$vectors_dir/ed25519vectors.json" \
		"$wycheproof_dir/testvectors_v1/ed25519_test.json")
	cat <<'EOF'

#endif /* BURROW_TESTS_ED25519_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
