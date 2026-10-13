#!/bin/sh
# Regenerates tests/ecdsa_test_gen.h: the files Go's crypto/ecdsa tests read.
# testdata/SigVer.rsp.bz2 goes in as it is, since the test reads it through
# compress/bzip2 here too. testdata/det-keygen.json goes in as its curve, seed
# and PKCS #8 in hex. The Wycheproof vectors Go fetches as a module when the
# test runs go in with the public key already out of its DER, as Go's x509
# would have taken it, since there is no x509 here yet. Go's own VerifyASN1 has
# to give what each Wycheproof test expects or this stops, so a vector Go would
# fail never gets in.
#
# Needs the network for the module, and the Go on PATH should be the release
# the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/ecdsa_test_gen.h"
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
	"crypto"
	"crypto/ecdsa"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"

	_ "crypto/sha256"
	_ "crypto/sha3"
	_ "crypto/sha512"
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

// hex as up to three C string literals in braces, since a few of the
// signatures are longer than the 4095 characters a C99 compiler has to take in
// one literal.
func parts(s string) string {
	var p []string
	for len(s) > 4000 {
		p = append(p, lit(s[:4000]))
		s = s[4000:]
	}
	p = append(p, lit(s))
	if len(p) > 3 {
		log.Fatalf("hex of %d characters does not fit", len(s))
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

// The files and hashes TestECDSAWycheproof reads, in its curveAndHashes, put
// in order since Go ranges over a map.
var wycheproofFiles = []string{
	"ecdsa_secp224r1_sha224_test.json",
	"ecdsa_secp224r1_sha256_test.json",
	"ecdsa_secp224r1_sha3_224_test.json",
	"ecdsa_secp224r1_sha3_256_test.json",
	"ecdsa_secp224r1_sha3_512_test.json",
	"ecdsa_secp224r1_sha512_test.json",
	"ecdsa_secp256r1_sha256_test.json",
	"ecdsa_secp256r1_sha3_256_test.json",
	"ecdsa_secp256r1_sha3_512_test.json",
	"ecdsa_secp256r1_sha512_test.json",
	"ecdsa_secp384r1_sha256_test.json",
	"ecdsa_secp384r1_sha384_test.json",
	"ecdsa_secp384r1_sha3_384_test.json",
	"ecdsa_secp384r1_sha3_512_test.json",
	"ecdsa_secp384r1_sha512_test.json",
	"ecdsa_secp521r1_sha3_512_test.json",
	"ecdsa_secp521r1_sha512_test.json",
}

// wycheproof.ParseHash, as Go and as the CryptoHash it is here.
var hashes = map[string]struct {
	h crypto.Hash
	c string
}{
	"SHA-224":  {crypto.SHA224, "CRYPTO_SHA224"},
	"SHA-256":  {crypto.SHA256, "CRYPTO_SHA256"},
	"SHA-384":  {crypto.SHA384, "CRYPTO_SHA384"},
	"SHA-512":  {crypto.SHA512, "CRYPTO_SHA512"},
	"SHA3-224": {crypto.SHA3_224, "CRYPTO_SHA3_224"},
	"SHA3-256": {crypto.SHA3_256, "CRYPTO_SHA3_256"},
	"SHA3-384": {crypto.SHA3_384, "CRYPTO_SHA3_384"},
	"SHA3-512": {crypto.SHA3_512, "CRYPTO_SHA3_512"},
}

func main() {
	sigVer, detKeygen, wycheproofDir := os.Args[1], os.Args[2], os.Args[3]

	z, err := os.ReadFile(sigVer)
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println("/* testdata/SigVer.rsp.bz2. */")
	fmt.Println("static const unsigned char gen_sigver_rsp_bz2[] = {")
	for i := 0; i < len(z); i += 12 {
		fmt.Print("   ")
		for _, c := range z[i:min(i+12, len(z))] {
			fmt.Printf(" 0x%02x,", c)
		}
		fmt.Println()
	}
	fmt.Println("};")
	fmt.Println()

	// TestKeyGenerationVectors.
	data, err := os.ReadFile(detKeygen)
	if err != nil {
		log.Fatal(err)
	}
	var vectors []struct {
		Curve string
		Seed  []byte
		PKCS8 []byte `json:"private_key_pkcs8"`
	}
	if err := json.Unmarshal(data, &vectors); err != nil {
		log.Fatal(err)
	}
	fmt.Println("/* testdata/det-keygen.json: the curve, and the seed and the PKCS #8 in hex. */")
	fmt.Println("static const GenKeyGen gen_keygen[] = {")
	for _, v := range vectors {
		fmt.Printf("    {%s, %s, %s},\n", lit(v.Curve), lit(hex.EncodeToString(v.Seed)),
			lit(hex.EncodeToString(v.PKCS8)))
	}
	fmt.Println("};")
	fmt.Println()

	// TestECDSAWycheproof. Go takes the key from the DER and so does this.
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
				log.Fatalf("%s: test group %d invalid DER encoded public key: %v", file, g+1, err)
			}
			pub, ok := k.(*ecdsa.PublicKey)
			if !ok {
				log.Fatalf("%s: test group %d: unexpected key type %T", file, g+1, k)
			}
			q, err := pub.Bytes()
			if err != nil {
				log.Fatal(err)
			}
			h, ok := hashes[tg.Sha]
			if !ok {
				log.Fatalf("%s: unexpected hash %q", file, tg.Sha)
			}
			fmt.Fprintf(&groups, "    {%s, %s, %s, %s},\n", lit(file), lit(pub.Curve.Params().Name),
				h.c, lit(hex.EncodeToString(q)))
			for _, tv := range tg.Tests {
				var want bool
				switch tv.Result {
				case "valid":
					want = true
				case "invalid":
					want = false
				case "acceptable":
					// Go's test passes no flags, so a flag would stop it.
					if len(tv.Flags) > 0 {
						log.Fatalf("%s #%d: unspecified flag: %q", file, tv.TcId, tv.Flags[0])
					}
					want = true
				default:
					log.Fatalf("%s #%d: unexpected result %q", file, tv.TcId, tv.Result)
				}
				hh := h.h.New()
				hh.Write(unhex(tv.Msg))
				if got := ecdsa.VerifyASN1(pub, hh.Sum(nil), unhex(tv.Sig)); got != want {
					log.Fatalf("%s #%d: Go's VerifyASN1 = %v, want %v", file, tv.TcId, got, want)
				}
				fmt.Fprintf(&tests, "    {%d, %d, %s, %s, %s, %v},\n", ngroups, tv.TcId,
					lit(tv.Comment), lit(tv.Msg), parts(tv.Sig), want)
			}
			ngroups++
		}
	}
	fmt.Println("/* The Wycheproof test groups: the file, the curve, the hash and the public")
	fmt.Println(" * key, uncompressed, in hex. */")
	fmt.Println("static const GenWycheproofGroup gen_wycheproof_groups[] = {")
	fmt.Print(groups.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The Wycheproof tests: the group, the tcId, the comment, the message in hex,")
	fmt.Println(" * the signature in hex in up to three pieces, and whether the signature is")
	fmt.Println(" * valid. */")
	fmt.Println("static const GenWycheproof gen_wycheproof[] = {")
	fmt.Print(tests.String())
	fmt.Println("};")
}
GO

{
	cat <<EOF
/* Generated by tools/gen-ecdsa-tests.sh; do not edit.
 *
 * What Go's crypto/ecdsa tests read, from $(go env GOVERSION): SigVer.rsp.bz2,
 * det-keygen.json and github.com/c2sp/wycheproof at
 * $wycheproof_version. */

#ifndef BURROW_TESTS_ECDSA_TEST_GEN_H
#define BURROW_TESTS_ECDSA_TEST_GEN_H

#include "burrow/crypto.h"

#include <stdbool.h>

typedef struct {
    const char *curve, *seed, *pkcs8;
} GenKeyGen;

typedef struct {
    const char *file, *curve;
    CryptoHash hash;
    const char *pub;
} GenWycheproofGroup;

typedef struct {
    int group, tc_id;
    const char *comment, *msg;
    const char *sig[3];
    bool want;
} GenWycheproof;

EOF
	(cd "$tmp/gen" && GO111MODULE=off go run main.go \
		"$goroot/src/crypto/ecdsa/testdata/SigVer.rsp.bz2" \
		"$goroot/src/crypto/ecdsa/testdata/det-keygen.json" \
		"$wycheproof_dir/testvectors_v1")
	cat <<'EOF'

#endif /* BURROW_TESTS_ECDSA_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
