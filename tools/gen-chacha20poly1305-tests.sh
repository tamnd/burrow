#!/bin/sh
# Regenerates tests/chacha20poly1305_test_gen.h: the test vectors of
# golang.org/x/crypto's chacha20, internal/poly1305 and chacha20poly1305, at
# the version Go vendors into the standard library, and the Wycheproof vectors
# of crypto/internal/cryptotest's TestChaCha20Poly1305Wycheproof, from the
# module version Go's tests pin.
#
# The vector files are Go source, so they are compiled as they are into a
# program that prints them. Every vector goes through x/crypto itself first and
# this stops if any of them does not give what it says, so a vector x/crypto
# would fail never gets in. The Poly1305 vectors that start from a state other
# than zero can only be checked from inside the package, and are carried over
# without that check.
#
# Needs the network for the module, and the Go on PATH should be the release
# the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/chacha20poly1305_test_gen.h"
goroot=$(go env GOROOT)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The version the standard library vendors.
version=$(sed -n 's/^# golang\.org\/x\/crypto \(.*\)$/\1/p' "$goroot/src/vendor/modules.txt")
dir=$( (cd "$tmp" && go mod download -json "golang.org/x/crypto@$version") |
	sed -n 's/^[[:space:]]*"Dir": "\(.*\)",$/\1/p')

# The version Go's tests pin, from cryptotest/wycheproof/schemaversion.go.
wycheproof_version=$(sed -n 's/^const wycheproofVersion = "\(.*\)"$/\1/p' \
	"$goroot/src/crypto/internal/cryptotest/wycheproof/schemaversion.go")
wycheproof_dir=$( (cd "$tmp" && go mod download -json "github.com/c2sp/wycheproof@$wycheproof_version") |
	sed -n 's/^[[:space:]]*"Dir": "\(.*\)",$/\1/p')

mkdir "$tmp/gen"
sed 's/^package chacha20$/package main/' "$dir/chacha20/vectors_test.go" \
	>"$tmp/gen/chacha20_vectors.go"
sed 's/^package poly1305$/package main/' "$dir/internal/poly1305/vectors_test.go" \
	>"$tmp/gen/poly1305_vectors.go"
sed 's/^package chacha20poly1305$/package main/' \
	"$dir/chacha20poly1305/chacha20poly1305_vectors_test.go" >"$tmp/gen/aead_vectors.go"
cat >"$tmp/gen/go.mod" <<EOF
module gen

go 1.24

require golang.org/x/crypto $version
EOF
cat >"$tmp/gen/main.go" <<'GO'
package main

import (
	"bytes"
	"crypto/cipher"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/crypto/chacha20"
	"golang.org/x/crypto/chacha20poly1305"
	"golang.org/x/crypto/poly1305"
)

// poly1305's vectors are of this type, which its poly1305_test.go has.
type test struct {
	in    string
	key   string
	tag   string
	state string
}

// The longest literal C99 promises to take is 4095 characters, so a long
// hex string goes in as several, which the tests join.
const partLen = 4000

var maxParts = 1

func lit(s string) string {
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < ' ' || c > '~' || c == '"' || c == '\\' || c == '?' {
			log.Fatalf("unexpected character %q in %q", c, s)
		}
	}
	if len(s) > 4095 {
		log.Fatalf("literal of %d characters is too long", len(s))
	}
	return `"` + s + `"`
}

// A C string literal of any text, for the Wycheproof comments, which have
// quotes and question marks in them.
func cstr(s string) string {
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

func split(s string) []string {
	var p []string
	for len(s) > partLen {
		p = append(p, lit(s[:partLen]))
		s = s[partLen:]
	}
	return append(p, lit(s))
}

func count(s string) {
	if n := len(split(s)); n > maxParts {
		maxParts = n
	}
}

func parts(s string) string {
	return "{" + strings.Join(split(s), ", ") + "}"
}

func unhex(s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		log.Fatalf("invalid hex %q: %v", s, err)
	}
	return b
}

func main() {
	for _, v := range testVectors {
		count(v.input)
		count(v.output)
	}
	for _, v := range testData {
		count(v.in)
	}
	for _, v := range chacha20Poly1305Tests {
		count(v.plaintext)
		count(v.aad)
		count(v.out)
	}

	fmt.Printf("#define GEN_HEX_PARTS %d\n\n", maxParts)
	fmt.Printf(`typedef struct {
    const char *nonce, *key;
    const char *input[GEN_HEX_PARTS], *output[GEN_HEX_PARTS];
} GenChacha20Vector;

typedef struct {
    const char *in[GEN_HEX_PARTS];
    const char *key, *tag;
    bool has_state;
    uint64_t state[3];
} GenPoly1305Vector;

typedef struct {
    const char *plaintext[GEN_HEX_PARTS], *aad[GEN_HEX_PARTS];
    const char *key, *nonce;
    const char *out[GEN_HEX_PARTS];
} GenChacha20Poly1305Vector;

`)

	// chacha20's testVectors, which TestNoOverlap and the rest go through.
	fmt.Printf("static const GenChacha20Vector gen_chacha20_vectors[] = {\n")
	for i, v := range testVectors {
		s, err := chacha20.NewUnauthenticatedCipher(unhex(v.key), unhex(v.nonce))
		if err != nil {
			log.Fatalf("chacha20 #%d: %v", i, err)
		}
		in := unhex(v.input)
		got := make([]byte, len(in))
		s.XORKeyStream(got, in)
		if hex.EncodeToString(got) != v.output {
			log.Fatalf("chacha20 #%d: x/crypto disagrees", i)
		}
		fmt.Printf("    {%s, %s, %s, %s},\n", lit(v.nonce), lit(v.key), parts(v.input),
			parts(v.output))
	}
	fmt.Printf("};\n\n")

	// poly1305's testData.
	fmt.Printf("static const GenPoly1305Vector gen_poly1305_vectors[] = {\n")
	for i, v := range testData {
		var key [32]byte
		copy(key[:], unhex(v.key))
		var tag [16]byte
		copy(tag[:], unhex(v.tag))
		var state [3]uint64
		if v.state != "" {
			buf := unhex(v.state)
			if len(buf) != 3*8 {
				log.Fatalf("poly1305 #%d: incorrect state length", i)
			}
			state = [3]uint64{
				binary.BigEndian.Uint64(buf[16:24]),
				binary.BigEndian.Uint64(buf[8:16]),
				binary.BigEndian.Uint64(buf[0:8]),
			}
		}
		if state == [3]uint64{} {
			var got [16]byte
			poly1305.Sum(&got, unhex(v.in), &key)
			if got != tag {
				log.Fatalf("poly1305 #%d: x/crypto disagrees", i)
			}
		}
		fmt.Printf("    {%s, %s, %s, %v, {UINT64_C(0x%016x), UINT64_C(0x%016x), UINT64_C(0x%016x)}},\n",
			parts(v.in), lit(v.key), lit(v.tag), state != [3]uint64{}, state[0], state[1],
			state[2])
	}
	fmt.Printf("};\n\n")

	// chacha20poly1305's chacha20Poly1305Tests, for both nonce sizes.
	fmt.Printf("static const GenChacha20Poly1305Vector gen_chacha20poly1305_vectors[] = {\n")
	for i, v := range chacha20Poly1305Tests {
		var aead cipher.AEAD
		var err error
		switch len(unhex(v.nonce)) {
		case chacha20poly1305.NonceSize:
			aead, err = chacha20poly1305.New(unhex(v.key))
		case chacha20poly1305.NonceSizeX:
			aead, err = chacha20poly1305.NewX(unhex(v.key))
		default:
			log.Fatalf("chacha20poly1305 #%d: wrong nonce length", i)
		}
		if err != nil {
			log.Fatalf("chacha20poly1305 #%d: %v", i, err)
		}
		ct := aead.Seal(nil, unhex(v.nonce), unhex(v.plaintext), unhex(v.aad))
		if hex.EncodeToString(ct) != v.out {
			log.Fatalf("chacha20poly1305 #%d: x/crypto disagrees", i)
		}
		pt, err := aead.Open(nil, unhex(v.nonce), ct, unhex(v.aad))
		if err != nil || !bytes.Equal(pt, unhex(v.plaintext)) {
			log.Fatalf("chacha20poly1305 #%d: x/crypto does not open it", i)
		}
		fmt.Printf("    {%s, %s, %s, %s, %s},\n", parts(v.plaintext), parts(v.aad), lit(v.key),
			lit(v.nonce), parts(v.out))
	}
	fmt.Printf("};\n\n")

	wycheproof(os.Args[1])
}

// Whether f panics, which is what wycheproof.MustPanic wants.
func panics(f func()) (panicked bool) {
	defer func() {
		if recover() != nil {
			panicked = true
		}
	}()
	f()
	return false
}

// The vectors of TestChaCha20Poly1305Wycheproof, each put through what the
// test does with it.
func wycheproof(dir string) {
	fmt.Printf(`typedef struct {
    const char *file;
    int tc_id;
    const char *comment;
    const char *key, *iv, *aad, *msg, *ct, *tag;
    bool want_pass;
} GenWycheproof;

`)
	fmt.Printf("/* The Wycheproof tests: the file, the tcId, the comment, the key, the IV,\n")
	fmt.Printf(" * the additional data, the message, the ciphertext and the tag in hex, and\n")
	fmt.Printf(" * whether the test is valid. */\n")
	fmt.Printf("static const GenWycheproof gen_wycheproof[] = {\n")
	for _, tc := range []struct {
		file      string
		newCipher func([]byte) (cipher.AEAD, error)
	}{
		{"chacha20_poly1305_test.json", chacha20poly1305.New},
		{"xchacha20_poly1305_test.json", chacha20poly1305.NewX},
	} {
		data, err := os.ReadFile(filepath.Join(dir, "testvectors_v1", tc.file))
		if err != nil {
			log.Fatal(err)
		}
		var wp struct {
			TestGroups []struct {
				Tests []struct {
					TcId    int      `json:"tcId"`
					Comment string   `json:"comment"`
					Key     string   `json:"key"`
					Iv      string   `json:"iv"`
					Aad     string   `json:"aad"`
					Msg     string   `json:"msg"`
					Ct      string   `json:"ct"`
					Tag     string   `json:"tag"`
					Result  string   `json:"result"`
					Flags   []string `json:"flags"`
				} `json:"tests"`
			} `json:"testGroups"`
		}
		if err := json.Unmarshal(data, &wp); err != nil {
			log.Fatal(err)
		}
		for _, tg := range wp.TestGroups {
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
						log.Fatalf("%s #%d: unspecified flag: %q", tc.file, tv.TcId, f)
					}
				default:
					log.Fatalf("%s #%d: unexpected result %q", tc.file, tv.TcId, tv.Result)
				}
				fail := func(format string, args ...any) {
					log.Fatalf("%s #%d: x/crypto: %s", tc.file, tv.TcId, fmt.Sprintf(format, args...))
				}
				aead, err := tc.newCipher(unhex(tv.Key))
				if err != nil {
					fail("failed to construct cipher: %s", err)
				}
				iv, tag, ct, msg, aad := unhex(tv.Iv), unhex(tv.Tag), unhex(tv.Ct), unhex(tv.Msg), unhex(tv.Aad)
				ctWithTag := append(ct, tag...)
				if len(iv) != aead.NonceSize() {
					if !panics(func() { aead.Seal(nil, iv, msg, aad) }) {
						fail("Seal: expected panic but didn't get one")
					}
					if !panics(func() { aead.Open(nil, iv, ctWithTag, aad) }) {
						fail("Open: expected panic but didn't get one")
					}
				} else {
					genCT := aead.Seal(nil, iv, msg, aad)
					genMsg, err := aead.Open(nil, iv, genCT, aad)
					if err != nil || !bytes.Equal(genMsg, msg) {
						fail("the generated ciphertext does not open to the message")
					}
					msg2, err := aead.Open(nil, iv, ctWithTag, aad)
					switch {
					case !wantPass && err == nil:
						fail("decryption succeeded when it should've failed")
					case wantPass && err != nil:
						fail("decryption failed: %s", err)
					case wantPass && (!bytes.Equal(genCT, ctWithTag) || !bytes.Equal(msg, msg2)):
						fail("the ciphertext or the message does not match")
					}
				}
				fmt.Printf("    {%s, %d, %s, %s, %s, %s, %s, %s, %s, %v},\n", cstr(tc.file),
					tv.TcId, cstr(tv.Comment), cstr(tv.Key), cstr(tv.Iv), cstr(tv.Aad), cstr(tv.Msg),
					cstr(tv.Ct), cstr(tv.Tag), wantPass)
			}
		}
	}
	fmt.Printf("};\n")
}
GO

{
	cat <<EOF
/* Generated by tools/gen-chacha20poly1305-tests.sh; do not edit.
 *
 * The test vectors of golang.org/x/crypto/chacha20, internal/poly1305 and
 * chacha20poly1305 at $version, which $(go env GOVERSION) vendors, and the
 * Wycheproof vectors of TestChaCha20Poly1305Wycheproof, from
 * github.com/c2sp/wycheproof at $wycheproof_version. */

#ifndef BURROW_TESTS_CHACHA20POLY1305_TEST_GEN_H
#define BURROW_TESTS_CHACHA20POLY1305_TEST_GEN_H

#include <stdbool.h>
#include <stdint.h>

EOF
	(cd "$tmp/gen" && GOFLAGS=-mod=mod go run . "$wycheproof_dir")
	cat <<'EOF'

#endif /* BURROW_TESTS_CHACHA20POLY1305_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
