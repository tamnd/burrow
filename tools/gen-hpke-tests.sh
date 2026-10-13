#!/bin/sh
# Regenerates tests/hpke_test_gen.h: the vectors of Go's crypto/hpke tests, from
# testdata/rfc9180.json and testdata/hpke-pq.json in the GOROOT.
#
# Only what TestVectors runs goes in. It skips every mode but the base one, the
# X448 and ML-KEM-512 KEMs and the TurboSHAKE KDFs, so those vectors are left
# out here. The rest have to pass Go's own TestVectors first, and every key,
# encryption and export a recipient alone can check goes through crypto/hpke
# again before it is printed, so a vector Go would fail never gets in.
#
# The Go on PATH should be the release the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/hpke_test_gen.h"
goroot=$(go env GOROOT)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

go test -count=1 -run '^TestVectors$' crypto/hpke >/dev/null

mkdir "$tmp/gen"
cat >"$tmp/gen/go.mod" <<EOF
module gen

go 1.26
EOF
cat >"$tmp/gen/main.go" <<'GO'
package main

import (
	"bytes"
	"crypto/hpke"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
)

type vector struct {
	Mode        uint16 `json:"mode"`
	KEM         uint16 `json:"kem_id"`
	KDF         uint16 `json:"kdf_id"`
	AEAD        uint16 `json:"aead_id"`
	Info        string `json:"info"`
	IkmE        string `json:"ikmE"`
	IkmR        string `json:"ikmR"`
	SkRm        string `json:"skRm"`
	PkRm        string `json:"pkRm"`
	Enc         string `json:"enc"`
	Encryptions []struct {
		Aad   string `json:"aad"`
		Ct    string `json:"ct"`
		Nonce string `json:"nonce"`
		Pt    string `json:"pt"`
	} `json:"encryptions"`
	Exports []struct {
		Context string `json:"exporter_context"`
		L       int    `json:"L"`
		Value   string `json:"exported_value"`
	} `json:"exports"`
	AccEncryptions string `json:"encryptions_accumulated"`
	AccExports     string `json:"exports_accumulated"`
}

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

func unhex(s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		log.Fatalf("invalid hex %q: %v", s, err)
	}
	return b
}

func skipped(v vector) bool {
	return v.Mode != 0 || v.KEM == 0x0021 || v.KEM == 0x0040 || v.KDF == 0x0012 ||
		v.KDF == 0x0013
}

// check goes through what a recipient can check on its own: the keys, and the
// encryptions and exports that are listed rather than accumulated.
func check(name string, v vector) {
	fail := func(format string, args ...any) {
		log.Fatalf("%s: %s", name, fmt.Sprintf(format, args...))
	}
	kem, err := hpke.NewKEM(v.KEM)
	if err != nil {
		fail("%v", err)
	}
	kdf, err := hpke.NewKDF(v.KDF)
	if err != nil {
		fail("%v", err)
	}
	aead, err := hpke.NewAEAD(v.AEAD)
	if err != nil {
		fail("%v", err)
	}
	pk, err := kem.NewPublicKey(unhex(v.PkRm))
	if err != nil {
		fail("%v", err)
	}
	if !bytes.Equal(pk.Bytes(), unhex(v.PkRm)) {
		fail("public key does not go back to the same bytes")
	}
	derived, err := kem.DeriveKeyPair(unhex(v.IkmR))
	if err != nil {
		fail("%v", err)
	}
	if !bytes.Equal(derived.PublicKey().Bytes(), unhex(v.PkRm)) {
		fail("DeriveKeyPair gives another public key")
	}
	sk, err := kem.NewPrivateKey(unhex(v.SkRm))
	if err != nil {
		fail("%v", err)
	}
	r, err := hpke.NewRecipient(unhex(v.Enc), sk, kdf, aead, unhex(v.Info))
	if err != nil {
		fail("%v", err)
	}
	for i, e := range v.Encryptions {
		pt, err := r.Open(unhex(e.Aad), unhex(e.Ct))
		if err != nil || !bytes.Equal(pt, unhex(e.Pt)) {
			fail("encryption %d does not open", i)
		}
	}
	for i, e := range v.Exports {
		got, err := r.Export(string(unhex(e.Context)), e.L)
		if err != nil || !bytes.Equal(got, unhex(e.Value)) {
			fail("export %d differs", i)
		}
	}
}

func main() {
	fmt.Printf(`typedef struct {
    const char *aad, *ct, *nonce, *pt;
} GenHpkeEncryption;

typedef struct {
    const char *context;
    int l;
    const char *value;
} GenHpkeExport;

typedef struct {
    const char *file;
    uint16_t kem, kdf, aead;
    const char *info, *ikm_e, *ikm_r, *sk_rm, *pk_rm, *enc;
    const GenHpkeEncryption *encryptions;
    int n_encryptions;
    const GenHpkeExport *exports;
    int n_exports;
    const char *acc_encryptions, *acc_exports;
} GenHpkeVector;
`)
	var rows []string
	for _, file := range []string{"rfc9180", "hpke-pq"} {
		data, err := os.ReadFile(filepath.Join(os.Args[1], file+".json"))
		if err != nil {
			log.Fatal(err)
		}
		var vectors []vector
		if err := json.Unmarshal(data, &vectors); err != nil {
			log.Fatal(err)
		}
		id := strings.ReplaceAll(file, "-", "_")
		for i, v := range vectors {
			if skipped(v) {
				continue
			}
			name := fmt.Sprintf("%s #%d", file, i)
			check(name, v)
			encs, exps := "NULL", "NULL"
			if len(v.Encryptions) > 0 {
				encs = fmt.Sprintf("gen_hpke_%s_%d_encryptions", id, i)
				fmt.Printf("\nstatic const GenHpkeEncryption %s[] = {\n", encs)
				for _, e := range v.Encryptions {
					fmt.Printf("    {%s, %s, %s, %s},\n", lit(e.Aad), lit(e.Ct), lit(e.Nonce),
						lit(e.Pt))
				}
				fmt.Printf("};\n")
			}
			if len(v.Exports) > 0 {
				exps = fmt.Sprintf("gen_hpke_%s_%d_exports", id, i)
				fmt.Printf("\nstatic const GenHpkeExport %s[] = {\n", exps)
				for _, e := range v.Exports {
					fmt.Printf("    {%s, %d, %s},\n", lit(e.Context), e.L, lit(e.Value))
				}
				fmt.Printf("};\n")
			}
			rows = append(rows, fmt.Sprintf(
				"    {%s, 0x%04x, 0x%04x, 0x%04x,\n     %s,\n     %s,\n     %s,\n     %s,\n     %s,\n     %s,\n     %s, %d, %s, %d,\n     %s, %s},\n",
				lit(file), v.KEM, v.KDF, v.AEAD, lit(v.Info), lit(v.IkmE), lit(v.IkmR),
				lit(v.SkRm), lit(v.PkRm), lit(v.Enc), encs, len(v.Encryptions), exps,
				len(v.Exports), lit(v.AccEncryptions), lit(v.AccExports)))
		}
	}
	fmt.Printf("\nstatic const GenHpkeVector gen_hpke_vectors[] = {\n%s};\n", strings.Join(rows, ""))
}
GO

{
	cat <<EOF
/* Generated by tools/gen-hpke-tests.sh; do not edit.
 *
 * The vectors of crypto/hpke's TestVectors, from testdata/rfc9180.json and
 * testdata/hpke-pq.json of $(go env GOVERSION), with the ones it skips left out. */

#ifndef BURROW_TESTS_HPKE_TEST_GEN_H
#define BURROW_TESTS_HPKE_TEST_GEN_H

#include <stddef.h>
#include <stdint.h>

EOF
	(cd "$tmp/gen" && go run . "$goroot/src/crypto/hpke/testdata")
	cat <<'EOF'

#endif /* BURROW_TESTS_HPKE_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
