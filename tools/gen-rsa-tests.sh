#!/bin/sh
# Regenerates tests/rsa_test_gen.h: the keys and vectors Go's crypto/rsa tests
# use, and the files they read. The keys are PEM in Go's test files and go in
# here as their numbers, as Go's x509 parses them, since there is no x509 here
# yet. testdata/pss-vect.txt.bz2 goes in as it is, since the test reads it
# through compress/bzip2 here too. The Wycheproof vectors Go fetches as a module
# when the test runs go in with their keys taken apart the same way, and each
# one has to give what the test expects when Go runs it or this stops, so a
# vector Go would fail never gets in.
#
# The generator runs as a test file laid over crypto/rsa with go test -overlay,
# so that it reads the keys and tables of Go's own tests rather than copies of
# them. The few values that live inside a test function are read out of the
# source.
#
# Needs the network for the module, and the Go on PATH should be the release
# the port follows.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/rsa_test_gen.h"
goroot=$(go env GOROOT)
version=$(go env GOVERSION)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The version Go's tests pin, from cryptotest/wycheproof/schemaversion.go.
wycheproof_version=$(sed -n 's/^const wycheproofVersion = "\(.*\)"$/\1/p' \
	"$goroot/src/crypto/internal/cryptotest/wycheproof/schemaversion.go")
wycheproof_dir=$( (cd "$tmp" && go mod download -json "github.com/c2sp/wycheproof@$wycheproof_version") |
	sed -n 's/^[[:space:]]*"Dir": "\(.*\)",$/\1/p')

cat >"$tmp/gen_test.go" <<'GO'
package rsa_test

import (
	"crypto"
	"crypto/internal/cryptotest/wycheproof"
	"crypto/rsa"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/asn1"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"slices"
	"strconv"
	"strings"
	"testing"

	_ "crypto/md5"
	_ "crypto/sha1"
	_ "crypto/sha256"
	_ "crypto/sha3"
	_ "crypto/sha512"
)

// C string literal of s, with octal escapes so nothing runs into the next
// character.
func bxLit(s string) string {
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

// s as up to three C string literals in braces, since some of the values are
// longer than the 4095 characters a C99 compiler has to take in one literal.
func bxParts(t *testing.T, s string) string {
	var p []string
	for len(s) > 4000 {
		p = append(p, bxLit(s[:4000]))
		s = s[4000:]
	}
	p = append(p, bxLit(s))
	if len(p) > 3 {
		t.Fatalf("value of %d characters does not fit", len(s))
	}
	return "{" + strings.Join(p, ", ") + "}"
}

func bxHex(b []byte) string { return bxLit(hex.EncodeToString(b)) }

// The CryptoHash of h.
func bxHash(t *testing.T, h crypto.Hash) string {
	names := map[crypto.Hash]string{
		crypto.MD5:        "CRYPTO_MD5",
		crypto.SHA1:       "CRYPTO_SHA1",
		crypto.SHA224:     "CRYPTO_SHA224",
		crypto.SHA256:     "CRYPTO_SHA256",
		crypto.SHA384:     "CRYPTO_SHA384",
		crypto.SHA512:     "CRYPTO_SHA512",
		crypto.SHA512_224: "CRYPTO_SHA512_224",
		crypto.SHA512_256: "CRYPTO_SHA512_256",
		crypto.SHA3_224:   "CRYPTO_SHA3_224",
		crypto.SHA3_256:   "CRYPTO_SHA3_256",
		crypto.SHA3_384:   "CRYPTO_SHA3_384",
		crypto.SHA3_512:   "CRYPTO_SHA3_512",
	}
	n, ok := names[h]
	if !ok {
		t.Fatalf("no CryptoHash for %v", h)
	}
	return n
}

// A private key as a GenKey: its numbers in hex, each as many bytes as it
// needs.
func bxKey(t *testing.T, name string, k *rsa.PrivateKey) string {
	if len(k.Primes) != 2 {
		t.Fatalf("%s has %d primes", name, len(k.Primes))
	}
	if k.Precomputed.Dp == nil {
		t.Fatalf("%s is not precomputed", name)
	}
	return fmt.Sprintf("{%s, %s, %d, %s, %s, %s, %s, %s, %s}", bxLit(name),
		bxHex(k.N.Bytes()), k.E, bxHex(k.D.Bytes()), bxHex(k.Primes[0].Bytes()),
		bxHex(k.Primes[1].Bytes()), bxHex(k.Precomputed.Dp.Bytes()),
		bxHex(k.Precomputed.Dq.Bytes()), bxHex(k.Precomputed.Qinv.Bytes()))
}

// A public key as a GenKey, with no private half.
func bxPub(name string, k *rsa.PublicKey) string {
	return fmt.Sprintf("{%s, %s, %d, \"\", \"\", \"\", \"\", \"\", \"\"}", bxLit(name),
		bxHex(k.N.Bytes()), k.E)
}

// The body of the function named fn in src, up to the next function.
func bxFunc(t *testing.T, src, fn string) string {
	i := strings.Index(src, "\nfunc "+fn+"(")
	if i < 0 {
		t.Fatalf("no func %s", fn)
	}
	body := src[i+1:]
	if j := strings.Index(body, "\nfunc "); j >= 0 {
		body = body[:j]
	}
	return body
}

// The PEM that follows the n-th backquote after marker in the function fn.
func bxPEM(t *testing.T, src, fn, marker string) string {
	body := bxFunc(t, src, fn)
	i := strings.Index(body, marker+"`")
	if i < 0 {
		t.Fatalf("no %s` in %s", marker, fn)
	}
	s := body[i+len(marker)+1:]
	return s[:strings.IndexByte(s, '`')]
}

// The []byte{...} given to name in the function fn.
func bxBytes(t *testing.T, src, fn, name string) []byte {
	body := bxFunc(t, src, fn)
	i := strings.Index(body, name+" := []byte{")
	if i < 0 {
		t.Fatalf("no %s in %s", name, fn)
	}
	s := body[i+len(name)+len(" := []byte{"):]
	s = s[:strings.IndexByte(s, '}')]
	var b []byte
	for _, f := range strings.FieldsFunc(s, func(r rune) bool {
		return r == ',' || r == ' ' || r == '\n' || r == '\t'
	}) {
		v, err := strconv.ParseUint(f, 0, 8)
		if err != nil {
			t.Fatalf("%s in %s: %v", name, fn, err)
		}
		b = append(b, byte(v))
	}
	return b
}

// The string literal right after call in the function fn.
func bxString(t *testing.T, src, fn, call string) string {
	body := bxFunc(t, src, fn)
	i := strings.Index(body, call+`"`)
	if i < 0 {
		t.Fatalf("no %s in %s", call, fn)
	}
	s := body[i+len(call):]
	q, err := strconv.QuotedPrefix(s)
	if err != nil {
		t.Fatal(err)
	}
	v, err := strconv.Unquote(q)
	if err != nil {
		t.Fatal(err)
	}
	return v
}

func bxRead(t *testing.T, name string) string {
	b, err := os.ReadFile(name)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// The value after "key = " on each line of a NIST-style file, in order, with
// the line it was on.
type bxLine struct {
	n    int
	k, v string
}

func bxLines(t *testing.T, name string) []bxLine {
	var out []bxLine
	for i, line := range strings.Split(bxRead(t, name), "\n") {
		if len(line) == 0 || line[0] == '#' {
			continue
		}
		k, v, _ := strings.Cut(line, " = ")
		out = append(out, bxLine{i + 1, k, v})
	}
	return out
}

func TestBurrowGen(t *testing.T) {
	out := os.Getenv("BURROW_GEN_OUT")
	if out == "" {
		t.Skip("BURROW_GEN_OUT is not set")
	}
	t.Setenv("GODEBUG", "rsa1024min=0")
	var w strings.Builder
	p := func(format string, args ...any) { fmt.Fprintf(&w, format, args...) }

	rsaTest := bxRead(t, "rsa_test.go")
	pssTest := bxRead(t, "pss_test.go")
	pkcs1Test := bxRead(t, "pkcs1v15_test.go")
	pkcs22Test := bxRead(t, "../internal/fips140/rsa/pkcs1v22_test.go")

	// The keys.
	p("/* The keys of rsa_test.go and pkcs1v15_test.go, as x509 parses them. */\n")
	p("static const GenKey gen_keys[] = {\n")
	keys := []struct {
		name string
		k    *rsa.PrivateKey
	}{
		{"test512Key", test512Key},
		{"test512KeyTwo", test512KeyTwo},
		{"test1024Key", test1024Key},
		{"test2048Key", test2048Key},
		{"test3072Key", test3072Key},
		{"test4096Key", test4096Key},
		{"TestGnuTLSKey", parseKey(testingKey(bxPEM(t, rsaTest, "TestGnuTLSKey", "priv := parseKey(testingKey(")))},
		{"TestPSmallerThanQ", parseKey(testingKey(bxPEM(t, rsaTest, "TestPSmallerThanQ", "k := parseKey(testingKey(")))},
		{"TestLargeSizeDifference/k1", parseKey(testingKey(bxPEM(t, rsaTest, "TestLargeSizeDifference", "k1 := parseKey(testingKey(")))},
		{"TestLargeSizeDifference/k2", parseKey(testingKey(bxPEM(t, rsaTest, "TestLargeSizeDifference", "k2 := parseKey(testingKey(")))},
	}
	for _, k := range keys {
		p("    %s,\n", bxKey(t, k.name, k.k))
	}
	p("    %s,\n", bxPub("TestShortPKCS1v15Signature", parsePublicKey(
		bxPEM(t, pkcs1Test, "TestShortPKCS1v15Signature", "pub := parsePublicKey("))))
	p("};\n\n")

	// The values inside test functions.
	p("/* Values that live inside the test functions. */\n")
	for _, v := range []struct {
		name string
		b    []byte
	}{
		{"gen_2decrypt_oaep_msg", bxBytes(t, rsaTest, "Test2DecryptOAEP", "msg")},
		{"gen_2decrypt_oaep_in", bxBytes(t, rsaTest, "Test2DecryptOAEP", "in")},
		{"gen_pss_openssl_sig", bxBytes(t, pssTest, "TestPSSOpenSSL", "sig")},
		{"gen_emsa_pss_msg", bxBytes(t, pkcs22Test, "TestEMSAPSS", "msg")},
		{"gen_emsa_pss_salt", bxBytes(t, pkcs22Test, "TestEMSAPSS", "salt")},
		{"gen_emsa_pss_expected", bxBytes(t, pkcs22Test, "TestEMSAPSS", "expected")},
	} {
		p("static const char %s[] = %s;\n", v.name, bxHex(v.b))
	}
	for _, v := range []struct{ name, s string }{
		{"gen_overlong_ciphertext", bxString(t, pkcs1Test, "TestOverlongMessagePKCS1v15", "ciphertext := decodeBase64(")},
		{"gen_unpadded_msg", bxString(t, pkcs1Test, "TestUnpaddedSignature", "msg := []byte(")},
		{"gen_unpadded_sig", bxString(t, pkcs1Test, "TestUnpaddedSignature", "expectedSig := decodeBase64(")},
		{"gen_short_pkcs1v15_sig", bxString(t, pkcs1Test, "TestShortPKCS1v15Signature", "sig, err := hex.DecodeString(")},
	} {
		p("static const char %s[] = %s;\n", v.name, bxLit(v.s))
	}
	p("\n")

	// testEncryptOAEPData.
	p("/* testEncryptOAEPData, in hex as rsa_test.go has it. */\n")
	p("static const GenOaepMessage gen_oaep_messages[] = {\n")
	var oaepKeys []string
	m := 0
	for _, k := range testEncryptOAEPData {
		oaepKeys = append(oaepKeys, fmt.Sprintf("{%s, %d, %s, %d, %d}", bxLit(k.modulus),
			k.e, bxLit(k.d), m, len(k.msgs)))
		for _, msg := range k.msgs {
			p("    {%s, %s, %s},\n", bxHex(msg.in), bxHex(msg.seed), bxHex(msg.out))
			m++
		}
	}
	p("};\n\nstatic const GenOaepKey gen_oaep_keys[] = {\n")
	for _, k := range oaepKeys {
		p("    %s,\n", k)
	}
	p("};\n\n")

	// The PKCS #1 v1.5 tables.
	for _, tab := range []struct {
		name  string
		tests []DecryptPKCS1v15Test
	}{
		{"decryptPKCS1v15Tests", decryptPKCS1v15Tests},
		{"decryptPKCS1v15SessionKeyTests", decryptPKCS1v15SessionKeyTests},
	} {
		p("/* %s. */\n", tab.name)
		p("static const GenInOut gen_%s[] = {\n", tab.name)
		for _, v := range tab.tests {
			p("    {%s, %s},\n", bxLit(v.in), bxLit(v.out))
		}
		p("};\n\n")
	}
	p("/* signPKCS1v15Tests. */\n")
	p("static const GenInOut gen_signPKCS1v15Tests[] = {\n")
	for _, v := range signPKCS1v15Tests {
		p("    {%s, %s},\n", bxLit(v.in), bxLit(v.out))
	}
	p("};\n\n")

	// The DER prefixes, as TestHashPrefixes works them out.
	prefixes := []struct {
		h   crypto.Hash
		oid asn1.ObjectIdentifier
	}{
		{crypto.MD5, asn1.ObjectIdentifier{1, 2, 840, 113549, 2, 5}},
		{crypto.SHA1, asn1.ObjectIdentifier{1, 3, 14, 3, 2, 26}},
		{crypto.SHA224, asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 4}},
		{crypto.SHA256, asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 1}},
		{crypto.SHA384, asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 2}},
		{crypto.SHA512, asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 3}},
		{crypto.SHA512_224, asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 5}},
		{crypto.SHA512_256, asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 6}},
		{crypto.SHA3_224, asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 7}},
		{crypto.SHA3_256, asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 8}},
		{crypto.SHA3_384, asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 9}},
		{crypto.SHA3_512, asn1.ObjectIdentifier{2, 16, 840, 1, 101, 3, 4, 2, 10}},
	}
	p("/* The DER prefix of each hash, from its OID, as TestHashPrefixes makes it. */\n")
	p("static const GenPrefix gen_prefixes[] = {\n")
	for _, v := range prefixes {
		want, err := asn1.Marshal(struct {
			HashAlgorithm pkix.AlgorithmIdentifier
			Hash          []byte
		}{
			HashAlgorithm: pkix.AlgorithmIdentifier{
				Algorithm:  v.oid,
				Parameters: asn1.NullRawValue,
			},
			Hash: make([]byte, v.h.Size()),
		})
		if err != nil {
			t.Fatal(err)
		}
		p("    {%s, %s},\n", bxHash(t, v.h), bxHex(want[:len(want)-v.h.Size()]))
	}
	p("};\n\n")

	// The Miller-Rabin and GCD tests, with the line each ends on.
	p("/* crypto/internal/fips140/rsa/testdata/miller_rabin_tests.txt. */\n")
	p("static const GenMillerRabin gen_miller_rabin[] = {\n")
	var possiblyPrime bool
	var W string
	for _, l := range bxLines(t, "../internal/fips140/rsa/testdata/miller_rabin_tests.txt") {
		switch l.k {
		case "Result":
			switch l.v {
			case "Composite":
				possiblyPrime = false
			case "PossiblyPrime":
				possiblyPrime = true
			default:
				t.Fatalf("unknown result %q on line %d", l.v, l.n)
			}
		case "W":
			W = l.v
		case "B":
			p("    {%d, %t, %s, %s},\n", l.n, possiblyPrime, bxLit(W), bxLit(l.v))
		default:
			t.Fatalf("unknown key %q on line %d", l.k, l.n)
		}
	}
	p("};\n\n")
	p("/* crypto/internal/fips140/rsa/testdata/gcd_lcm_tests.txt. */\n")
	p("static const GenGcdLcm gen_gcd_lcm[] = {\n")
	var GCD, A, B string
	for _, l := range bxLines(t, "../internal/fips140/rsa/testdata/gcd_lcm_tests.txt") {
		switch l.k {
		case "GCD":
			GCD = l.v
		case "A":
			A = l.v
		case "B":
			B = l.v
		case "LCM":
			p("    {%d, %s, %s, %s, %s},\n", l.n, bxLit(GCD), bxLit(A), bxLit(B), bxLit(l.v))
		default:
			t.Fatalf("unknown key %q on line %d", l.k, l.n)
		}
	}
	p("};\n\n")

	// det-keygen.json.
	var vectors []struct {
		Bits  int
		Seed  []byte
		PKCS8 []byte `json:"private_key_pkcs8"`
	}
	if err := json.Unmarshal([]byte(bxRead(t, "testdata/det-keygen.json")), &vectors); err != nil {
		t.Fatal(err)
	}
	p("/* testdata/det-keygen.json: the size, the seed and the key it makes. */\n")
	p("static const GenKeyGen gen_keygen[] = {\n")
	for i, v := range vectors {
		k, err := x509.ParsePKCS8PrivateKey(v.PKCS8)
		if err != nil {
			t.Fatal(err)
		}
		p("    {%d, %s, %s},\n", v.Bits, bxHex(v.Seed), bxKey(t, fmt.Sprint(i), k.(*rsa.PrivateKey)))
	}
	p("};\n\n")

	// The prime candidates BenchmarkGenerateKey reads.
	for _, bits := range []int{2048, 3072, 4096} {
		p("/* testdata/keygen%d.txt, without its comments. */\n", bits)
		p("static const char *const gen_keygen%d[] = {\n", bits)
		for _, line := range strings.Split(bxRead(t, fmt.Sprintf("testdata/keygen%d.txt", bits)), "\n") {
			if line != "" && line[0] != '#' {
				p("    %s,\n", bxLit(line))
			}
		}
		p("};\n\n")
	}

	// pss-vect.txt.bz2.
	pss := bxRead(t, "testdata/pss-vect.txt.bz2")
	p("/* testdata/pss-vect.txt.bz2. */\n")
	p("static const unsigned char gen_pss_vect_bz2[] = {")
	for i := 0; i < len(pss); i++ {
		if i%12 == 0 {
			p("\n   ")
		}
		p(" 0x%02x,", pss[i])
	}
	p("\n};\n\n")

	wycheproofOAEP(t, p)
	wycheproofPKCS1Decrypt(t, p)
	wycheproofPKCS1Sig(t, p)
	wycheproofPSS(t, p)

	if err := os.WriteFile(out, []byte(w.String()), 0o644); err != nil {
		t.Fatal(err)
	}
}

func bxPriv(t *testing.T, file, der string) *rsa.PrivateKey {
	k, err := x509.ParsePKCS8PrivateKey(wycheproof.MustDecodeHex(der))
	if err != nil {
		t.Fatalf("%s: %v", file, err)
	}
	return k.(*rsa.PrivateKey)
}

func bxPublic(t *testing.T, file, der string) *rsa.PublicKey {
	k, err := x509.ParsePKCS1PublicKey(wycheproof.MustDecodeHex(der))
	if err != nil {
		t.Fatalf("%s: %v", file, err)
	}
	return k
}

func wycheproofOAEP(t *testing.T, p func(string, ...any)) {
	flagsShouldPass := map[string]bool{
		"Constructed":            true,
		"EncryptionWithLabel":    true,
		"SmallIntegerCiphertext": true,
	}
	var groups, tests []string
	for _, file := range []string{
		"rsa_oaep_2048_sha1_mgf1sha1_test.json",
		"rsa_oaep_2048_sha224_mgf1sha1_test.json",
		"rsa_oaep_2048_sha224_mgf1sha224_test.json",
		"rsa_oaep_2048_sha256_mgf1sha1_test.json",
		"rsa_oaep_2048_sha256_mgf1sha256_test.json",
		"rsa_oaep_2048_sha384_mgf1sha1_test.json",
		"rsa_oaep_2048_sha384_mgf1sha384_test.json",
		"rsa_oaep_2048_sha512_224_mgf1sha1_test.json",
		"rsa_oaep_2048_sha512_224_mgf1sha512_224_test.json",
		"rsa_oaep_2048_sha512_mgf1sha1_test.json",
		"rsa_oaep_2048_sha512_mgf1sha512_test.json",
		"rsa_oaep_3072_sha256_mgf1sha1_test.json",
		"rsa_oaep_3072_sha256_mgf1sha256_test.json",
		"rsa_oaep_3072_sha512_256_mgf1sha1_test.json",
		"rsa_oaep_3072_sha512_256_mgf1sha512_256_test.json",
		"rsa_oaep_3072_sha512_mgf1sha1_test.json",
		"rsa_oaep_3072_sha512_mgf1sha512_test.json",
		"rsa_oaep_4096_sha256_mgf1sha1_test.json",
		"rsa_oaep_4096_sha256_mgf1sha256_test.json",
		"rsa_oaep_4096_sha512_mgf1sha1_test.json",
		"rsa_oaep_4096_sha512_mgf1sha512_test.json",
		"rsa_oaep_misc_test.json",
	} {
		var testdata wycheproof.RsaesOaepDecryptSchemaV1Json
		wycheproof.LoadVectorFile(t, file, &testdata)
		for _, tg := range testdata.TestGroups {
			priv := bxPriv(t, file, tg.PrivateKeyPkcs8)
			hash := wycheproof.ParseHash(tg.Sha)
			mgfHash := wycheproof.ParseHash(tg.MgfSha)
			g := len(groups)
			groups = append(groups, fmt.Sprintf("{%s, %s, %s, %s}", bxLit(file),
				bxHash(t, hash), bxHash(t, mgfHash), bxKey(t, file, priv)))
			for _, tv := range tg.Tests {
				ct := wycheproof.MustDecodeHex(tv.Ct)
				label := wycheproof.MustDecodeHex(tv.Label)
				want := wycheproof.ShouldPass(t, tv.Result, tv.Flags, flagsShouldPass)
				plaintext, err := priv.Decrypt(nil, ct, &rsa.OAEPOptions{
					Hash: hash, MGFHash: mgfHash, Label: label,
				})
				if (err == nil) != want || (want && hex.EncodeToString(plaintext) != tv.Msg) {
					t.Fatalf("%s: Go fails it: %v", wycheproof.TestName(file, tv), err)
				}
				tests = append(tests, fmt.Sprintf("{%d, %d, %s, %s, %s, %s, %t}", g,
					tv.TcId, bxLit(tv.Comment), bxLit(tv.Msg), bxLit(tv.Label),
					bxParts(t, tv.Ct), want))
			}
		}
	}
	p("/* The Wycheproof vectors TestRSAOAEPDecryptWycheproof runs. */\n")
	p("static const GenOaepGroup gen_wycheproof_oaep_groups[] = {\n")
	for _, g := range groups {
		p("    %s,\n", g)
	}
	p("};\n\nstatic const GenWycheproof gen_wycheproof_oaep[] = {\n")
	for _, v := range tests {
		p("    %s,\n", v)
	}
	p("};\n\n")
}

func wycheproofPKCS1Decrypt(t *testing.T, p func(string, ...any)) {
	var groups, tests []string
	for _, file := range []string{
		"rsa_pkcs1_2048_test.json",
		"rsa_pkcs1_3072_test.json",
		"rsa_pkcs1_4096_test.json",
	} {
		var testdata wycheproof.RsaesPkcs1DecryptSchemaV1Json
		wycheproof.LoadVectorFile(t, file, &testdata)
		for _, tg := range testdata.TestGroups {
			priv := bxPriv(t, file, tg.PrivateKeyPkcs8)
			g := len(groups)
			groups = append(groups, fmt.Sprintf("{%s, %s}", bxLit(file), bxKey(t, file, priv)))
			for _, tv := range tg.Tests {
				want := wycheproof.ShouldPass(t, tv.Result, tv.Flags, nil)
				plaintext, err := rsa.DecryptPKCS1v15(nil, priv, wycheproof.MustDecodeHex(tv.Ct))
				if (err == nil) != want || (want && hex.EncodeToString(plaintext) != tv.Msg) {
					t.Fatalf("%s: Go fails it: %v", wycheproof.TestName(file, tv), err)
				}
				tests = append(tests, fmt.Sprintf("{%d, %d, %s, %s, \"\", %s, %t}", g,
					tv.TcId, bxLit(tv.Comment), bxLit(tv.Msg), bxParts(t, tv.Ct), want))
			}
		}
	}
	p("/* The Wycheproof vectors TestRSAPKCS1DecryptWycheproof runs. */\n")
	p("static const GenDecryptGroup gen_wycheproof_pkcs1_decrypt_groups[] = {\n")
	for _, g := range groups {
		p("    %s,\n", g)
	}
	p("};\n\nstatic const GenWycheproof gen_wycheproof_pkcs1_decrypt[] = {\n")
	for _, v := range tests {
		p("    %s,\n", v)
	}
	p("};\n\n")
}

func wycheproofPKCS1Sig(t *testing.T, p func(string, ...any)) {
	// modsAndHashes, in order, since Go ranges over a map.
	modsAndHashes := []struct {
		mod    int
		hashes []string
	}{
		{2048, []string{"sha224", "sha256", "sha384", "sha512", "sha512_224", "sha512_256",
			"sha3_224", "sha3_256", "sha3_384", "sha3_512"}},
		{3072, []string{"sha256", "sha384", "sha512", "sha512_256", "sha3_256", "sha3_384",
			"sha3_512"}},
		{4096, []string{"sha256", "sha384", "sha512", "sha512_256"}},
		{8192, []string{"sha256", "sha384", "sha512"}},
	}
	flagsShouldPass := map[string]bool{
		"MissingNull": false,
	}
	var groups, tests []string
	for _, mh := range modsAndHashes {
		for _, h := range mh.hashes {
			file := fmt.Sprintf("rsa_signature_%d_%s_test.json", mh.mod, h)
			var testdata wycheproof.RsassaPkcs1VerifySchemaV1Json
			wycheproof.LoadVectorFile(t, file, &testdata)
			for _, tg := range testdata.TestGroups {
				hash := wycheproof.ParseHash(tg.Sha)
				pub := bxPublic(t, file, tg.PublicKeyAsn)
				g := len(groups)
				groups = append(groups, fmt.Sprintf("{%s, %s, 0, %s}", bxLit(file),
					bxHash(t, hash), bxPub(file, pub)))
				for _, tv := range tg.Tests {
					hh := hash.New()
					hh.Write(wycheproof.MustDecodeHex(tv.Msg))
					err := rsa.VerifyPKCS1v15(pub, hash, hh.Sum(nil), wycheproof.MustDecodeHex(tv.Sig))
					want := wycheproof.ShouldPass(t, tv.Result, tv.Flags, flagsShouldPass)
					if (err == nil) != want {
						t.Fatalf("%s: Go fails it: %v", wycheproof.TestName(file, tv), err)
					}
					tests = append(tests, fmt.Sprintf("{%d, %d, %s, %s, \"\", %s, %t}", g,
						tv.TcId, bxLit(tv.Comment), bxLit(tv.Msg), bxParts(t, tv.Sig), want))
				}
			}
		}
	}
	p("/* The Wycheproof vectors TestRSAPKCS1SignaturesWycheproof runs. */\n")
	p("static const GenVerifyGroup gen_wycheproof_pkcs1_sig_groups[] = {\n")
	for _, g := range groups {
		p("    %s,\n", g)
	}
	p("};\n\nstatic const GenWycheproof gen_wycheproof_pkcs1_sig[] = {\n")
	for _, v := range tests {
		p("    %s,\n", v)
	}
	p("};\n\n")
}

func wycheproofPSS(t *testing.T, p func(string, ...any)) {
	// filesOverrideToPassZeroSLen, in order, since Go ranges over a map.
	files := []struct {
		file        string
		overrideIDs []int
	}{
		{"rsa_pss_2048_sha1_mgf1_20_test.json", []int{46, 47, 48, 49, 50, 51}},
		{"rsa_pss_2048_sha256_mgf1_0_test.json", []int{67, 68, 69, 70}},
		{"rsa_pss_2048_sha256_mgf1_32_test.json", []int{67, 68, 69, 70, 71, 72}},
		{"rsa_pss_3072_sha256_mgf1_32_test.json", []int{67, 68, 69, 70, 71, 72}},
		{"rsa_pss_4096_sha256_mgf1_32_test.json", []int{67, 68, 69, 70, 71, 72}},
		{"rsa_pss_4096_sha512_mgf1_32_test.json", []int{136, 137, 138, 139, 140, 141}},
		{"rsa_pss_misc_test.json", nil},
	}
	var groups, tests []string
	for _, f := range files {
		file := f.file
		var testdata wycheproof.RsassaPssVerifySchemaV1Json
		wycheproof.LoadVectorFile(t, file, &testdata)
		for _, tg := range testdata.TestGroups {
			if file == "rsa_pss_misc_test.json" && tg.Sha != tg.MgfSha {
				continue
			}
			hash := wycheproof.ParseHash(tg.Sha)
			pub := bxPublic(t, file, tg.PublicKeyAsn)
			g := len(groups)
			groups = append(groups, fmt.Sprintf("{%s, %s, %d, %s}", bxLit(file),
				bxHash(t, hash), tg.SLen, bxPub(file, pub)))
			// Go's test makes opts again for each of its two runs, so both
			// verify with PSSSaltLengthAuto, and the override always applies.
			opts := &rsa.PSSOptions{Hash: hash, SaltLength: rsa.PSSSaltLengthAuto}
			for _, tv := range tg.Tests {
				hh := hash.New()
				hh.Write(wycheproof.MustDecodeHex(tv.Msg))
				err := rsa.VerifyPSS(pub, hash, hh.Sum(nil), wycheproof.MustDecodeHex(tv.Sig), opts)
				want := wycheproof.ShouldPass(t, tv.Result, tv.Flags, nil)
				if slices.Contains(f.overrideIDs, tv.TcId) {
					want = true
				}
				if (err == nil) != want {
					t.Fatalf("%s: Go fails it: %v", wycheproof.TestName(file, tv), err)
				}
				tests = append(tests, fmt.Sprintf("{%d, %d, %s, %s, \"\", %s, %t}", g,
					tv.TcId, bxLit(tv.Comment), bxLit(tv.Msg), bxParts(t, tv.Sig), want))
			}
		}
	}
	p("/* The Wycheproof vectors TestRSAPSSSignaturesWycheproof runs, with what\n")
	p(" * it wants after its overrides. */\n")
	p("static const GenVerifyGroup gen_wycheproof_pss_groups[] = {\n")
	for _, g := range groups {
		p("    %s,\n", g)
	}
	p("};\n\nstatic const GenWycheproof gen_wycheproof_pss[] = {\n")
	for _, v := range tests {
		p("    %s,\n", v)
	}
	p("};\n")
}
GO

cat >"$tmp/overlay.json" <<EOF
{"Replace": {"$goroot/src/crypto/rsa/zz_burrow_gen_test.go": "$tmp/gen_test.go"}}
EOF

(cd "$tmp" && BURROW_GEN_OUT="$tmp/body.h" GO111MODULE=off \
	go test -count=1 -timeout=0 -overlay "$tmp/overlay.json" -run '^TestBurrowGen$' crypto/rsa \
	-args -wycheproof-dir="$wycheproof_dir")
test -s "$tmp/body.h"

{
	cat <<EOF
/* Generated by tools/gen-rsa-tests.sh; do not edit.
 *
 * What Go's crypto/rsa tests use, from $version: the keys and tables of its
 * test files, the files in testdata and in crypto/internal/fips140/rsa's
 * testdata, and github.com/c2sp/wycheproof at
 * $wycheproof_version. */

#ifndef BURROW_TESTS_RSA_TEST_GEN_H
#define BURROW_TESTS_RSA_TEST_GEN_H

#include "burrow/crypto.h"

#include <stdbool.h>

/* A key's numbers in hex. A public key has the private ones empty. */
typedef struct {
    const char *name, *n;
    int e;
    const char *d, *p, *q, *dp, *dq, *qinv;
} GenKey;

typedef struct {
    const char *in, *seed, *out;
} GenOaepMessage;

/* A key of testEncryptOAEPData, with the modulus and d in hex as Go has them,
 * and where its messages start in gen_oaep_messages. */
typedef struct {
    const char *modulus;
    int e;
    const char *d;
    int msgs, nmsgs;
} GenOaepKey;

typedef struct {
    const char *in, *out;
} GenInOut;

typedef struct {
    CryptoHash hash;
    const char *prefix;
} GenPrefix;

typedef struct {
    int line;
    bool possibly_prime;
    const char *w, *b;
} GenMillerRabin;

typedef struct {
    int line;
    const char *gcd, *a, *b, *lcm;
} GenGcdLcm;

typedef struct {
    int bits;
    const char *seed;
    GenKey key;
} GenKeyGen;

typedef struct {
    const char *file;
    CryptoHash hash, mgf_hash;
    GenKey key;
} GenOaepGroup;

typedef struct {
    const char *file;
    GenKey key;
} GenDecryptGroup;

typedef struct {
    const char *file;
    CryptoHash hash;
    int salt_len;
    GenKey key;
} GenVerifyGroup;

/* One Wycheproof test: data is the ciphertext or the signature, in up to
 * three pieces. */
typedef struct {
    int group, tc_id;
    const char *comment, *msg, *label;
    const char *data[3];
    bool want;
} GenWycheproof;

EOF
	cat "$tmp/body.h"
	cat <<'EOF'

#endif /* BURROW_TESTS_RSA_TEST_GEN_H */
EOF
} >"$tmp/out.h"
mv "$tmp/out.h" "$out"
