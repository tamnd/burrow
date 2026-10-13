#!/bin/sh
# Regenerates tests/mldsa_test_gen.h: the Wycheproof vectors Go's crypto/mldsa
# tests fetch as a module when they run, the ACVP rejection known answer tests
# of crypto/internal/fips140/mldsa, and the messages BenchmarkSign signs.
#
# Everything a test feeds in goes in as it is. A signature a test only compares
# against goes in as its SHA-256, which keeps the header near the size of the
# others, and so does a public key a sign test only compares against. A
# comparison of hashes fails where the comparison of bytes would.
#
# Go's own crypto/mldsa has to give what each verify test and each sign test
# without randomness expects, or this stops, so a vector Go would fail never
# gets in. Signing with given randomness and the semi-expanded private keys are
# only reachable inside Go's FIPS module, so those vectors are carried over
# without that check. So are the key hashes of the known answer tests, which
# hash the semi-expanded key, but their signatures go through crypto/mldsa with
# an external μ first.
#
# Needs the network for the module, and the Go on PATH should be the release
# the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/mldsa_test_gen.h"
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
	"crypto/mldsa"
	"crypto/sha256"
	"crypto/sha3"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
)

// The most literals one value is split into, GEN_MLDSA_PARTS in the header.
const maxParts = 4

// C string literal of s, which is only ever hex, a comment or a name. A C99
// compiler only has to take 4095 characters in one.
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

// hex as up to maxParts C string literals in braces, since a signature is
// longer than one literal can be.
func parts(s string) string {
	n := len(s)
	var p []string
	for len(s) > 4000 {
		p = append(p, lit(s[:4000]))
		s = s[4000:]
	}
	p = append(p, lit(s))
	if len(p) > maxParts {
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

// The SHA-256 of b, as a literal.
func digest(b []byte) string {
	h := sha256.Sum256(b)
	return lit(hex.EncodeToString(h[:]))
}

// wycheproof.ShouldPass with no flags that pass, which is how Go's tests call
// it: an acceptable vector with a flag stops the test.
func shouldPass(where, result string, flags []string) bool {
	switch result {
	case "valid":
		return true
	case "invalid":
		return false
	case "acceptable":
		if len(flags) > 0 {
			log.Fatalf("%s: unspecified flag: %q", where, flags[0])
		}
		return true
	}
	log.Fatalf("%s: unexpected result %q", where, result)
	return false
}

func params(algorithm string) (int, mldsa.Parameters) {
	switch algorithm {
	case "ML-DSA-44":
		return 44, mldsa.MLDSA44()
	case "ML-DSA-65":
		return 65, mldsa.MLDSA65()
	case "ML-DSA-87":
		return 87, mldsa.MLDSA87()
	}
	log.Fatalf("unknown algorithm: %s", algorithm)
	return 0, mldsa.Parameters{}
}

func load(dir, file string, v any) {
	data, err := os.ReadFile(filepath.Join(dir, file))
	if err != nil {
		log.Fatal(err)
	}
	if err := json.Unmarshal(data, v); err != nil {
		log.Fatal(err)
	}
}

func str(p *string) string {
	if p == nil {
		return ""
	}
	return *p
}

type tv struct {
	TcId    int      `json:"tcId"`
	Comment string   `json:"comment"`
	Flags   []string `json:"flags"`
	Result  string   `json:"result"`
	Msg     *string  `json:"msg"`
	Ctx     *string  `json:"ctx"`
	Mu      *string  `json:"mu"`
	Rnd     *string  `json:"rnd"`
	Sig     string   `json:"sig"`
}

type file struct {
	Algorithm  string `json:"algorithm"`
	TestGroups []struct {
		PublicKey   any    `json:"publicKey"`
		PrivateSeed string `json:"privateSeed"`
		PrivateKey  string `json:"privateKey"`
		Tests       []tv   `json:"tests"`
	} `json:"testGroups"`
}

func pkOf(v any) []byte {
	if s, ok := v.(string); ok {
		return unhex(s)
	}
	return nil
}

func pkDigest(pk []byte) string {
	if pk == nil {
		return lit("")
	}
	return digest(pk)
}

// The bytes of an optional hex field, and whether it is there. An empty mu or
// rnd counts as not there, as it does in Go's tests.
func opt(p *string, emptyIsAbsent bool) ([]byte, bool) {
	if p == nil || (emptyIsAbsent && *p == "") {
		return nil, false
	}
	return unhex(*p), true
}

// runSignTest of mldsa_wycheproof_test.go, as a check that Go agrees with the
// vector.
func checkSign(where string, priv *mldsa.PrivateKey, t tv, pass bool) {
	msg, hasMsg := opt(t.Msg, false)
	opts := new(mldsa.Options)
	if hasMsg && t.Ctx != nil {
		opts.Context = string(unhex(*t.Ctx))
	}
	mu, hasMu := opt(t.Mu, true)
	if !hasMsg && !hasMu {
		log.Fatalf("%s: test vector has neither msg nor mu", where)
	}
	var sigMsg, sigMu []byte
	var errMsg, errMu error
	if hasMsg {
		sigMsg, errMsg = priv.SignDeterministic(msg, opts)
	}
	if hasMu {
		sigMu, errMu = priv.SignDeterministic(mu, crypto.MLDSAMu)
	}
	if errMsg != nil || errMu != nil {
		if pass {
			log.Fatalf("%s: Go fails a vector that should pass: %v %v", where, errMsg, errMu)
		}
		return
	}
	if !pass {
		log.Fatalf("%s: Go signs a vector that should fail", where)
	}
	sig := sigMsg
	if !hasMsg {
		sig = sigMu
	}
	if hasMsg && hasMu && !bytes.Equal(sigMsg, sigMu) {
		log.Fatalf("%s: Go's signatures of msg and mu disagree", where)
	}
	if !bytes.Equal(sig, unhex(t.Sig)) {
		log.Fatalf("%s: Go's signature differs", where)
	}
	if hasMsg {
		if err := mldsa.Verify(priv.PublicKey(), msg, sig, opts); err != nil {
			log.Fatalf("%s: Go does not verify its own signature: %v", where, err)
		}
	}
}

func main() {
	dir, goroot := os.Args[1], os.Args[2]
	var vgroups, verify, sgroups, sign, ngroups, noseed, kats, bench strings.Builder

	// TestVerifyWycheproof.
	ng := 0
	for _, name := range []string{"mldsa_44_verify_test.json", "mldsa_65_verify_test.json", "mldsa_87_verify_test.json"} {
		var f file
		load(dir, name, &f)
		k, p := params(f.Algorithm)
		for _, tg := range f.TestGroups {
			pkBytes := pkOf(tg.PublicKey)
			fmt.Fprintf(&vgroups, "    {%d, %s},\n", k, parts(hex.EncodeToString(pkBytes)))
			for _, t := range tg.Tests {
				where := fmt.Sprintf("%s #%d", name, t.TcId)
				pass := shouldPass(where, t.Result, t.Flags)
				pub, err := mldsa.NewPublicKey(p, pkBytes)
				if err == nil {
					opts := new(mldsa.Options)
					if t.Ctx != nil {
						opts.Context = string(unhex(*t.Ctx))
					}
					err = mldsa.Verify(pub, unhex(str(t.Msg)), unhex(t.Sig), opts)
				}
				if (err == nil) != pass {
					log.Fatalf("%s: Go's error is %v, should pass is %v", where, err, pass)
				}
				fmt.Fprintf(&verify, "    {%d, %d, %s, %s, %v, %s, %s, %v},\n", ng, t.TcId,
					lit(t.Comment), parts(str(t.Msg)), t.Ctx != nil, lit(str(t.Ctx)),
					parts(t.Sig), pass)
			}
			ng++
		}
	}

	// TestSignSeedWycheproof and TestMLDSASignSeedRandomizedWycheproof.
	ng = 0
	for _, name := range []string{"mldsa_44_sign_seed_test.json", "mldsa_65_sign_seed_test.json", "mldsa_87_sign_seed_test.json"} {
		var f file
		load(dir, name, &f)
		k, p := params(f.Algorithm)
		for _, tg := range f.TestGroups {
			pk := pkOf(tg.PublicKey)
			fmt.Fprintf(&sgroups, "    {%d, %s, %s},\n", k, lit(tg.PrivateSeed), pkDigest(pk))
			for _, t := range tg.Tests {
				where := fmt.Sprintf("%s #%d", name, t.TcId)
				pass := shouldPass(where, t.Result, t.Flags)
				randomized := slices.Contains(t.Flags, "Randomized")
				priv, err := mldsa.NewPrivateKey(p, unhex(tg.PrivateSeed))
				if err != nil {
					if pass {
						log.Fatalf("%s: NewPrivateKey: %v", where, err)
					}
				} else {
					if pk != nil && !bytes.Equal(priv.PublicKey().Bytes(), pk) {
						log.Fatalf("%s: public key mismatch", where)
					}
					if !randomized {
						checkSign(where, priv, t, pass)
					}
				}
				sig := lit("")
				if pass {
					sig = digest(unhex(t.Sig))
				}
				fmt.Fprintf(&sign, "    {%d, %d, %s, %v, %s, %v, %s, %s, %s, %s, %v, %v},\n",
					ng, t.TcId, lit(t.Comment), t.Msg != nil, parts(str(t.Msg)),
					t.Ctx != nil, lit(str(t.Ctx)), lit(str(t.Mu)), lit(str(t.Rnd)), sig,
					randomized, pass)
			}
			ng++
		}
	}

	// TestMLDSANoSeedWycheproof.
	ng = 0
	for _, name := range []string{"mldsa_44_sign_noseed_test.json", "mldsa_65_sign_noseed_test.json", "mldsa_87_sign_noseed_test.json"} {
		var f file
		load(dir, name, &f)
		k, _ := params(f.Algorithm)
		for _, tg := range f.TestGroups {
			fmt.Fprintf(&ngroups, "    {%d, %s, %s},\n", k, parts(tg.PrivateKey),
				pkDigest(pkOf(tg.PublicKey)))
			for _, t := range tg.Tests {
				where := fmt.Sprintf("%s #%d", name, t.TcId)
				fmt.Fprintf(&noseed, "    {%d, %d, %s, %v},\n", ng, t.TcId, lit(t.Comment),
					shouldPass(where, t.Result, t.Flags))
			}
			ng++
		}
	}

	// TestACVPRejectionKATs, from the table in its source.
	src, err := os.ReadFile(filepath.Join(goroot, "src/crypto/internal/fips140/mldsa/mldsa_test.go"))
	if err != nil {
		log.Fatal(err)
	}
	re := regexp.MustCompile(`\{\s*"([^"]+)",\s*"([0-9A-F]+)",\s*"([0-9A-F]+)",\s*"([0-9A-F]*)",\s*"([0-9A-F]+)",\s*NewPrivateKey(44|65|87), NewPublicKey(44|65|87),\s*\}`)
	n := 0
	for _, m := range re.FindAllStringSubmatch(string(src), -1) {
		if m[6] != m[7] {
			log.Fatalf("%s: mixed parameter sets", m[1])
		}
		_, p := params("ML-DSA-" + m[6])
		priv, err := mldsa.NewPrivateKey(p, unhex(m[2]))
		if err != nil {
			log.Fatalf("%s: %v", m[1], err)
		}
		pk := priv.PublicKey().Bytes()
		tr := sha3.SumSHAKE256(pk, 64)
		h := sha3.NewSHAKE256()
		h.Write(tr)
		h.Write(unhex(m[4]))
		mu := make([]byte, 64)
		h.Read(mu)
		sig, err := priv.SignDeterministic(mu, crypto.MLDSAMu)
		if err != nil {
			log.Fatalf("%s: %v", m[1], err)
		}
		if got := sha256.Sum256(sig); !bytes.Equal(got[:], unhex(m[5])) {
			log.Fatalf("%s: Go's signature hash differs", m[1])
		}
		fmt.Fprintf(&kats, "    {%s, %s, %s, %s, %s, %s},\n", m[6], lit(m[1]), lit(m[2]),
			lit(m[3]), lit(m[4]), lit(m[5]))
		n++
	}
	rows := regexp.MustCompile(`NewPrivateKey(44|65|87), NewPublicKey`).FindAllString(string(src), -1)
	if n == 0 || n != len(rows) {
		log.Fatalf("found %d of the %d known answer tests", n, len(rows))
	}

	// BenchmarkSign's messages.
	src, err = os.ReadFile(filepath.Join(goroot, "src/crypto/mldsa/mldsa_test.go"))
	if err != nil {
		log.Fatal(err)
	}
	list := regexp.MustCompile(`var benchmarkMessagesMLDSA(44|65|87) = \[\]string\{([^}]*)\}`)
	item := regexp.MustCompile(`"([^"\\]*)"`)
	lists := list.FindAllStringSubmatch(string(src), -1)
	if len(lists) != 3 {
		log.Fatalf("found %d benchmark message lists, not 3", len(lists))
	}
	for _, l := range lists {
		fmt.Fprintf(&bench, "static const char *const gen_mldsa_bench%s[] = {\n", l[1])
		for _, m := range item.FindAllStringSubmatch(l[2], -1) {
			fmt.Fprintf(&bench, "    %s,\n", lit(m[1]))
		}
		fmt.Fprintf(&bench, "};\n\n")
	}

	fmt.Println("/* The public keys of the groups of the verify tests: the parameter set and")
	fmt.Println(" * the key in hex. */")
	fmt.Println("static const GenMldsaVerifyGroup gen_mldsa_verify_groups[] = {")
	fmt.Print(vgroups.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The verify tests: the group, the tcId, the comment, the message in hex,")
	fmt.Println(" * whether there is a context and the context in hex, the signature in hex,")
	fmt.Println(" * and whether it should pass. */")
	fmt.Println("static const GenMldsaVerify gen_mldsa_verify[] = {")
	fmt.Print(verify.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The groups of the sign_seed tests: the parameter set, the seed in hex and")
	fmt.Println(" * the SHA-256 of the public key, or nothing when the group has none. */")
	fmt.Println("static const GenMldsaSeedGroup gen_mldsa_seed_groups[] = {")
	fmt.Print(sgroups.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The sign_seed tests: the group, the tcId, the comment, whether there is a")
	fmt.Println(" * message and the message in hex, whether there is a context and the context,")
	fmt.Println(" * μ and the randomness in hex or nothing, the SHA-256 of the signature when")
	fmt.Println(" * it should pass, whether the signature is randomized, and whether it should")
	fmt.Println(" * pass. */")
	fmt.Println("static const GenMldsaSign gen_mldsa_sign[] = {")
	fmt.Print(sign.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The groups of the sign_noseed tests: the parameter set, the semi-expanded")
	fmt.Println(" * private key in hex and the SHA-256 of the public key, or nothing. */")
	fmt.Println("static const GenMldsaNoseedGroup gen_mldsa_noseed_groups[] = {")
	fmt.Print(ngroups.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The sign_noseed tests: the group, the tcId, the comment, and whether it")
	fmt.Println(" * should pass. */")
	fmt.Println("static const GenMldsaNoseed gen_mldsa_noseed[] = {")
	fmt.Print(noseed.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* TestACVPRejectionKATs: the parameter set, the name, the seed, the SHA-256")
	fmt.Println(" * of the public key and the semi-expanded private key, the message that goes")
	fmt.Println(" * into μ, and the SHA-256 of the signature. */")
	fmt.Println("static const GenMldsaKat gen_mldsa_kats[] = {")
	fmt.Print(kats.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The messages of BenchmarkSign for each parameter set. */")
	fmt.Print(strings.TrimSuffix(bench.String(), "\n"))
}
GO

{
	cat <<EOF
/* Generated by tools/gen-mldsa-tests.sh; do not edit.
 *
 * The Wycheproof vectors Go's crypto/mldsa tests read, from $(go env GOVERSION)
 * and github.com/c2sp/wycheproof at $wycheproof_version, with the known answer
 * tests and benchmark messages of the same Go. */

#ifndef BURROW_TESTS_MLDSA_TEST_GEN_H
#define BURROW_TESTS_MLDSA_TEST_GEN_H

#include <stdbool.h>

#define GEN_MLDSA_PARTS 4

typedef struct {
    int params;
    const char *pk[GEN_MLDSA_PARTS];
} GenMldsaVerifyGroup;

typedef struct {
    int group, tc_id;
    const char *comment;
    const char *msg[GEN_MLDSA_PARTS];
    bool has_ctx;
    const char *ctx;
    const char *sig[GEN_MLDSA_PARTS];
    bool pass;
} GenMldsaVerify;

typedef struct {
    int params;
    const char *seed, *pk_sha256;
} GenMldsaSeedGroup;

typedef struct {
    int group, tc_id;
    const char *comment;
    bool has_msg;
    const char *msg[GEN_MLDSA_PARTS];
    bool has_ctx;
    const char *ctx, *mu, *rnd, *sig_sha256;
    bool randomized, pass;
} GenMldsaSign;

typedef struct {
    int params;
    const char *sk[GEN_MLDSA_PARTS];
    const char *pk_sha256;
} GenMldsaNoseedGroup;

typedef struct {
    int group, tc_id;
    const char *comment;
    bool pass;
} GenMldsaNoseed;

typedef struct {
    int params;
    const char *name, *seed, *key_sha256, *msg, *sig_sha256;
} GenMldsaKat;

EOF
	(cd "$tmp/gen" && GO111MODULE=off go run main.go "$wycheproof_dir/testvectors_v1" "$goroot")
	cat <<'EOF'

#endif /* BURROW_TESTS_MLDSA_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
