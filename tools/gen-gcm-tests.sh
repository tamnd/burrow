#!/bin/sh
# Regenerates tests/gcm_test_gen.h: the Wycheproof vectors Go's GCM test
# in crypto/cipher fetches as a module when it runs. Go's own AES-GCM has to do
# what the test expects of each one or this stops, so a vector Go would fail
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
out="$root/tests/gcm_test_gen.h"
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

func main() {
	wycheproofDir := os.Args[1]

	file := "aes_gcm_test.json"
	data, err := os.ReadFile(filepath.Join(wycheproofDir, file))
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
	var tests strings.Builder
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
					log.Fatalf("%s #%d: unspecified flag: %q", file, tv.TcId, f)
				}
			default:
				log.Fatalf("%s #%d: unexpected result %q", file, tv.TcId, tv.Result)
			}
			fail := func(format string, args ...any) {
				log.Fatalf("%s #%d: Go's GCM: %s", file, tv.TcId, fmt.Sprintf(format, args...))
			}
			aesCipher, err := aes.NewCipher(unhex(tv.Key))
			if err != nil {
				fail("failed to construct cipher: %s", err)
			}
			aead, err := cipher.NewGCM(aesCipher)
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
			fmt.Fprintf(&tests, "    {%s, %d, %s, %s, %s, %s, %s, %s, %s, %v},\n", lit(file), tv.TcId,
				lit(tv.Comment), lit(tv.Key), lit(tv.Iv), lit(tv.Aad), lit(tv.Msg), lit(tv.Ct),
				lit(tv.Tag), wantPass)
		}
	}
	fmt.Println("/* The Wycheproof tests: the file, the tcId, the comment, the key, the IV,")
	fmt.Println(" * the additional data, the message, the ciphertext and the tag in hex, and")
	fmt.Println(" * whether the test is valid. */")
	fmt.Println("static const GenWycheproof gen_wycheproof[] = {")
	fmt.Print(tests.String())
	fmt.Println("};")
}
GO

{
	cat <<EOF
/* Generated by tools/gen-gcm-tests.sh; do not edit.
 *
 * The Wycheproof vectors Go's crypto/cipher GCM test reads, from $(go env GOVERSION)
 * and github.com/c2sp/wycheproof at $wycheproof_version. */

#ifndef BURROW_TESTS_GCM_TEST_GEN_H
#define BURROW_TESTS_GCM_TEST_GEN_H

#include <stdbool.h>

typedef struct {
    const char *file;
    int tc_id;
    const char *comment;
    const char *key, *iv, *aad, *msg, *ct, *tag;
    bool want_pass;
} GenWycheproof;

EOF
	(cd "$tmp/gen" && GO111MODULE=off go run main.go "$wycheproof_dir/testvectors_v1")
	cat <<'EOF'

#endif /* BURROW_TESTS_GCM_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
