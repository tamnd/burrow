#!/bin/sh
# Regenerates tests/mlkem_test_gen.h: the Wycheproof vectors Go's crypto/mlkem
# tests fetch as a module when they run.
#
# Everything a test feeds in goes in as it is. What a test only compares
# against, the keys a seed expands to and the ciphertexts, goes in as its
# SHA-256, which keeps the header near the size of the others. A comparison of
# hashes fails where the comparison of bytes would.
#
# Go's own crypto/mlkem and mlkemtest have to give what each test expects or
# this stops, so a vector Go would fail never gets in. The expanded NIST form of
# a decapsulation key is only reachable inside Go's FIPS module, so those
# vectors and the expanded key of each seed are carried over without that
# check.
#
# Needs the network for the module, and the Go on PATH should be the release
# the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/mlkem_test_gen.h"
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
	"crypto/mlkem"
	"crypto/mlkem/mlkemtest"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
)

// C string literal of s, which is only ever hex or a comment. A C99 compiler
// only has to take 4095 characters in one.
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

// hex as up to two C string literals in braces, since an expanded
// ML-KEM-1024 decapsulation key is longer than one literal can be.
func parts(s string) string {
	n := len(s)
	var p []string
	for len(s) > 4000 {
		p = append(p, lit(s[:4000]))
		s = s[4000:]
	}
	p = append(p, lit(s))
	if len(p) > 2 {
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

// The SHA-256 of the bytes in hex s, as a literal.
func digest(s string) string {
	h := sha256.Sum256(unhex(s))
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

func params(where, set string) int {
	switch set {
	case "ML-KEM-768":
		return 768
	case "ML-KEM-1024":
		return 1024
	}
	log.Fatalf("%s: parameter set %s unsupported", where, set)
	return 0
}

type kem struct {
	bytes       func() []byte
	encapsulate func(m []byte) (k, c []byte, err error)
}

type dkey struct {
	bytes       func() []byte
	decapsulate func(c []byte) ([]byte, error)
	ek          func() []byte
}

func newEK(k int, b []byte) (*kem, error) {
	if k == 768 {
		ek, err := mlkem.NewEncapsulationKey768(b)
		if err != nil {
			return nil, err
		}
		return &kem{ek.Bytes, func(m []byte) ([]byte, []byte, error) {
			return mlkemtest.Encapsulate768(ek, m)
		}}, nil
	}
	ek, err := mlkem.NewEncapsulationKey1024(b)
	if err != nil {
		return nil, err
	}
	return &kem{ek.Bytes, func(m []byte) ([]byte, []byte, error) {
		return mlkemtest.Encapsulate1024(ek, m)
	}}, nil
}

func newDK(k int, seed []byte) (*dkey, error) {
	if k == 768 {
		dk, err := mlkem.NewDecapsulationKey768(seed)
		if err != nil {
			return nil, err
		}
		return &dkey{dk.Bytes, dk.Decapsulate, dk.EncapsulationKey().Bytes}, nil
	}
	dk, err := mlkem.NewDecapsulationKey1024(seed)
	if err != nil {
		return nil, err
	}
	return &dkey{dk.Bytes, dk.Decapsulate, dk.EncapsulationKey().Bytes}, nil
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

type tv struct {
	TcId    int      `json:"tcId"`
	Comment string   `json:"comment"`
	Flags   []string `json:"flags"`
	Result  string   `json:"result"`
	Seed    string   `json:"seed"`
	Ek      *string  `json:"ek"`
	Dk      string   `json:"dk"`
	M       string   `json:"m"`
	C       string   `json:"c"`
	K       *string  `json:"K"`
}

type file struct {
	TestGroups []struct {
		ParameterSet string `json:"parameterSet"`
		Tests        []tv   `json:"tests"`
	} `json:"testGroups"`
}

func str(p *string) string {
	if p == nil {
		return ""
	}
	return *p
}

func main() {
	dir := os.Args[1]
	var keygen, encaps, decaps, semi strings.Builder

	// TestKeyGenWycheproof.
	for _, name := range []string{"mlkem_768_keygen_seed_test.json", "mlkem_1024_keygen_seed_test.json"} {
		var f file
		load(dir, name, &f)
		for _, tg := range f.TestGroups {
			for _, t := range tg.Tests {
				where := fmt.Sprintf("%s #%d", name, t.TcId)
				k := params(where, tg.ParameterSet)
				dk, err := newDK(k, unhex(t.Seed))
				if err != nil {
					log.Fatalf("%s: %v", where, err)
				}
				if !bytes.Equal(dk.ek(), unhex(str(t.Ek))) {
					log.Fatalf("%s: encapsulation key mismatch", where)
				}
				fmt.Fprintf(&keygen, "    {%d, %d, %s, %s, %s, %s, %v},\n", k, t.TcId,
					lit(t.Comment), lit(t.Seed), digest(str(t.Ek)), digest(t.Dk),
					t.Result == "valid")
			}
		}
	}

	// TestMLKEMEncapsWycheproof.
	for _, name := range []string{"mlkem_768_encaps_test.json", "mlkem_1024_encaps_test.json"} {
		var f file
		load(dir, name, &f)
		for _, tg := range f.TestGroups {
			for _, t := range tg.Tests {
				where := fmt.Sprintf("%s #%d", name, t.TcId)
				k := params(where, tg.ParameterSet)
				pass := shouldPass(where, t.Result, t.Flags)
				ek, err := newEK(k, unhex(str(t.Ek)))
				if err == nil {
					var key, c []byte
					key, c, err = ek.encapsulate(unhex(t.M))
					if err == nil && (!bytes.Equal(c, unhex(t.C)) || !bytes.Equal(key, unhex(str(t.K)))) {
						log.Fatalf("%s: Go's ciphertext or shared key differs", where)
					}
				}
				if (err == nil) != pass {
					log.Fatalf("%s: Go's error is %v, should pass is %v", where, err, pass)
				}
				c, key := "", ""
				if pass {
					c, key = digest(t.C), lit(str(t.K))
				} else {
					c, key = lit(""), lit("")
				}
				fmt.Fprintf(&encaps, "    {%d, %d, %s, %s, %s, %s, %s, %v},\n", k, t.TcId,
					lit(t.Comment), parts(str(t.Ek)), lit(t.M), c, key, pass)
			}
		}
	}

	// TestMLKEMDecapsWycheproof.
	for _, name := range []string{"mlkem_768_test.json", "mlkem_1024_test.json"} {
		var f file
		load(dir, name, &f)
		for _, tg := range f.TestGroups {
			for _, t := range tg.Tests {
				where := fmt.Sprintf("%s #%d", name, t.TcId)
				k := params(where, tg.ParameterSet)
				pass := shouldPass(where, t.Result, t.Flags)
				dk, err := newDK(k, unhex(t.Seed))
				if err == nil {
					if t.Ek != nil && !bytes.Equal(dk.ek(), unhex(*t.Ek)) {
						log.Fatalf("%s: encapsulation key mismatch", where)
					}
					var key []byte
					key, err = dk.decapsulate(unhex(t.C))
					if err == nil && pass && !bytes.Equal(key, unhex(str(t.K))) {
						log.Fatalf("%s: Go's shared key differs", where)
					}
				}
				if err != nil && pass {
					log.Fatalf("%s: Go fails a vector that should pass: %v", where, err)
				}
				ek := lit("")
				if t.Ek != nil {
					ek = digest(*t.Ek)
				}
				fmt.Fprintf(&decaps, "    {%d, %d, %s, %s, %s, %s, %s, %v},\n", k, t.TcId,
					lit(t.Comment), lit(t.Seed), parts(t.C), ek, lit(str(t.K)), pass)
			}
		}
	}

	// TestMLKEMSemiExpandedDecapsWycheproof.
	for _, name := range []string{"mlkem_768_semi_expanded_decaps_test.json", "mlkem_1024_semi_expanded_decaps_test.json"} {
		var f file
		load(dir, name, &f)
		for _, tg := range f.TestGroups {
			for _, t := range tg.Tests {
				where := fmt.Sprintf("%s #%d", name, t.TcId)
				k := params(where, tg.ParameterSet)
				pass := shouldPass(where, t.Result, t.Flags)
				fmt.Fprintf(&semi, "    {%d, %d, %s, %s, %s, %s, %s, %v, %v},\n", k, t.TcId,
					lit(t.Comment), parts(t.Dk), parts(t.C), digest(str(t.Ek)),
					lit(str(t.K)), t.K != nil, pass)
			}
		}
	}

	fmt.Println("/* The keygen_seed tests: the parameter set, the tcId, the comment, the")
	fmt.Println(" * seed in hex, the SHA-256 of the encapsulation key and of the expanded")
	fmt.Println(" * decapsulation key, and whether the result is valid. */")
	fmt.Println("static const GenMlkemKeyGen gen_mlkem_keygen[] = {")
	fmt.Print(keygen.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The encaps tests: the parameter set, the tcId, the comment, the")
	fmt.Println(" * encapsulation key and m in hex, the SHA-256 of the ciphertext, the shared")
	fmt.Println(" * key, and whether it should pass. */")
	fmt.Println("static const GenMlkemEncaps gen_mlkem_encaps[] = {")
	fmt.Print(encaps.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The decaps tests: the parameter set, the tcId, the comment, the seed and")
	fmt.Println(" * the ciphertext in hex, the SHA-256 of the encapsulation key or nothing, the")
	fmt.Println(" * shared key, and whether it should pass. */")
	fmt.Println("static const GenMlkemDecaps gen_mlkem_decaps[] = {")
	fmt.Print(decaps.String())
	fmt.Println("};")
	fmt.Println()
	fmt.Println("/* The semi_expanded_decaps tests: the parameter set, the tcId, the comment,")
	fmt.Println(" * the expanded decapsulation key and the ciphertext in hex, the SHA-256 of")
	fmt.Println(" * the encapsulation key, the shared key, whether there is one, and whether")
	fmt.Println(" * it should pass. */")
	fmt.Println("static const GenMlkemSemi gen_mlkem_semi[] = {")
	fmt.Print(semi.String())
	fmt.Println("};")
}
GO

{
	cat <<EOF
/* Generated by tools/gen-mlkem-tests.sh; do not edit.
 *
 * The Wycheproof vectors Go's crypto/mlkem tests read, from $(go env GOVERSION)
 * and github.com/c2sp/wycheproof at $wycheproof_version. */

#ifndef BURROW_TESTS_MLKEM_TEST_GEN_H
#define BURROW_TESTS_MLKEM_TEST_GEN_H

#include <stdbool.h>

typedef struct {
    int params, tc_id;
    const char *comment, *seed, *ek_sha256, *dk_sha256;
    bool valid;
} GenMlkemKeyGen;

typedef struct {
    int params, tc_id;
    const char *comment;
    const char *ek[2];
    const char *m, *c_sha256, *k;
    bool pass;
} GenMlkemEncaps;

typedef struct {
    int params, tc_id;
    const char *comment, *seed;
    const char *c[2];
    const char *ek_sha256, *k;
    bool pass;
} GenMlkemDecaps;

typedef struct {
    int params, tc_id;
    const char *comment;
    const char *dk[2], *c[2];
    const char *ek_sha256, *k;
    bool has_k, pass;
} GenMlkemSemi;

EOF
	(cd "$tmp/gen" && GO111MODULE=off go run main.go "$wycheproof_dir/testvectors_v1")
	cat <<'EOF'

#endif /* BURROW_TESTS_MLKEM_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
