#!/bin/sh
# Regenerates tests/dsa_test_gen.h: the Wycheproof vectors Go's crypto/dsa
# tests fetch as a module when they run. Each public key goes in as its four
# integers, already out of the DER as Go's x509 would have taken them, since
# there is no x509 here yet. Go's own Verify, behind the verifyASN1 of its test,
# has to give what each test expects or this stops, so a vector Go would fail
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
out="$root/tests/dsa_test_gen.h"
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
	"crypto"
	"crypto/dsa"
	"crypto/x509"
	"encoding/asn1"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"math/big"
	"os"
	"path/filepath"
	"strings"

	_ "crypto/sha256"
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

// hex as up to three C string literals in braces, since a few of the messages
// are longer than one literal can be.
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

func hexInt(n *big.Int) string {
	return lit(hex.EncodeToString(n.Bytes()))
}

// verifyASN1 from Go's test, with encoding/asn1 in place of cryptobyte, which
// is not in the standard library. encoding/asn1 lets more through, bytes after
// the last field of a SEQUENCE for one, so the signature also has to be what
// encoding the two integers again gives, which is the DER cryptobyte wants.
func verifyASN1(pub *dsa.PublicKey, hash, sig []byte) bool {
	var rs struct{ R, S *big.Int }
	rest, err := asn1.Unmarshal(sig, &rs)
	if err != nil || len(rest) != 0 {
		return false
	}
	if der, err := asn1.Marshal(rs); err != nil || !bytes.Equal(der, sig) {
		return false
	}
	return dsa.Verify(pub, hash, rs.R, rs.S)
}

// The files TestDSAWycheproof reads, in its order.
var wycheproofFiles = []string{
	"dsa_2048_224_sha224_test.json",
	"dsa_2048_224_sha256_test.json",
	"dsa_2048_256_sha256_test.json",
	"dsa_3072_256_sha256_test.json",
}

// wycheproof.ParseHash, as Go and as the CryptoHash it is here.
var hashes = map[string]struct {
	h crypto.Hash
	c string
}{
	"SHA-224": {crypto.SHA224, "CRYPTO_SHA224"},
	"SHA-256": {crypto.SHA256, "CRYPTO_SHA256"},
}

func main() {
	wycheproofDir := os.Args[1]

	type test struct {
		TcId    int      `json:"tcId"`
		Comment string   `json:"comment"`
		Msg     string   `json:"msg"`
		Sig     string   `json:"sig"`
		Result  string   `json:"result"`
		Flags   []string `json:"flags"`
	}
	var groups, tests strings.Builder
	ngroups := 0
	for _, file := range wycheproofFiles {
		data, err := os.ReadFile(filepath.Join(wycheproofDir, file))
		if err != nil {
			log.Fatal(err)
		}
		var wp struct {
			TestGroups []struct {
				PublicKeyDer string `json:"publicKeyDer"`
				Sha          string `json:"sha"`
				Tests        []test `json:"tests"`
			} `json:"testGroups"`
		}
		if err := json.Unmarshal(data, &wp); err != nil {
			log.Fatal(err)
		}
		for g, tg := range wp.TestGroups {
			k, err := x509.ParsePKIXPublicKey(unhex(tg.PublicKeyDer))
			if err != nil {
				log.Fatalf("%s: test group %d: failed to parse DER encoded public key: %v", file, g+1, err)
			}
			pub, ok := k.(*dsa.PublicKey)
			if !ok {
				log.Fatalf("%s: test group %d: unexpected key type %T", file, g+1, k)
			}
			h, ok := hashes[tg.Sha]
			if !ok {
				log.Fatalf("%s: unexpected hash %q", file, tg.Sha)
			}
			fmt.Fprintf(&groups, "    {%s, %s, %s, %s, %s, %s},\n", lit(file), h.c,
				hexInt(pub.P), hexInt(pub.Q), hexInt(pub.G), hexInt(pub.Y))
			for _, tv := range tg.Tests {
				var want bool
				switch tv.Result {
				case "valid":
					want = true
				case "invalid":
					want = false
				case "acceptable":
					// Go's test passes MissingZero as false and stops on
					// any other flag.
					want = true
					for _, f := range tv.Flags {
						if f != "MissingZero" {
							log.Fatalf("%s #%d: unspecified flag: %q", file, tv.TcId, f)
						}
						want = false
					}
				default:
					log.Fatalf("%s #%d: unexpected result %q", file, tv.TcId, tv.Result)
				}
				hh := h.h.New()
				hh.Write(unhex(tv.Msg))
				hashed := hh.Sum(nil)[:pub.Q.BitLen()/8]
				if got := verifyASN1(pub, hashed, unhex(tv.Sig)); got != want {
					log.Fatalf("%s #%d: Go's Verify = %v, want %v", file, tv.TcId, got, want)
				}
				fmt.Fprintf(&tests, "    {%d, %d, %s, %s, %s, %v},\n", ngroups, tv.TcId,
					lit(tv.Comment), parts(tv.Msg), parts(tv.Sig), want)
			}
			ngroups++
		}
	}
	fmt.Println("/* The Wycheproof test groups: the file, the hash, and the P, Q, G and Y of")
	fmt.Println(" * the public key in hex. */")
	fmt.Println("static const GenWycheproofGroup gen_wycheproof_groups[] = {")
	fmt.Print(groups.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The Wycheproof tests: the group, the tcId, the comment, the message and")
	fmt.Println(" * the signature in hex, each in up to three pieces, and whether the")
	fmt.Println(" * signature is valid. */")
	fmt.Println("static const GenWycheproof gen_wycheproof[] = {")
	fmt.Print(tests.String())
	fmt.Println("};")
}
GO

{
	cat <<EOF
/* Generated by tools/gen-dsa-tests.sh; do not edit.
 *
 * The Wycheproof vectors Go's crypto/dsa tests read, from $(go env GOVERSION)
 * and github.com/c2sp/wycheproof at $wycheproof_version. */

#ifndef BURROW_TESTS_DSA_TEST_GEN_H
#define BURROW_TESTS_DSA_TEST_GEN_H

#include "burrow/crypto.h"

#include <stdbool.h>

typedef struct {
    const char *file;
    CryptoHash hash;
    const char *p, *q, *g, *y;
} GenWycheproofGroup;

typedef struct {
    int group, tc_id;
    const char *comment;
    const char *msg[3], *sig[3];
    bool want;
} GenWycheproof;

EOF
	(cd "$tmp/gen" && GO111MODULE=off go run main.go "$wycheproof_dir/testvectors_v1")
	cat <<'EOF'

#endif /* BURROW_TESTS_DSA_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
